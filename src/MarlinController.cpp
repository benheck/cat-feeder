#include "MarlinController.h"
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <sys/ioctl.h>
#include <thread>
#include <chrono>

MarlinController::MarlinController(const std::string& port, int baudrate) : stopReading(false) {

    marlinPort = open(port.c_str(), O_RDWR | O_NOCTTY | O_SYNC);

    if (marlinPort < 0) {
        perror("open");
        throw std::runtime_error("Failed to open serial port");
    }

    struct termios tty;
    memset(&tty, 0, sizeof tty);

    if (tcgetattr(marlinPort, &tty) != 0) {
        perror("tcgetattr");
        throw std::runtime_error("Failed to set termios");
    }

    cfsetospeed(&tty, B115200);
    cfsetispeed(&tty, B115200);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;     // 8-bit chars
    tty.c_iflag &= ~IGNBRK;         // disable break processing
    tty.c_lflag = 0;                // no signaling chars, no echo, no canonical processing
    tty.c_oflag = 0;                // no remapping, no delays
    tty.c_cc[VMIN]  = 0;            // read doesn't block
    tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout

    tty.c_iflag &= ~(IXON | IXOFF | IXANY); // shut off xon/xoff ctrl

    tty.c_cflag |= (CLOCAL | CREAD);// ignore modem controls, enable reading
    tty.c_cflag &= ~(PARENB | PARODD);      // shut off parity
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    tcflush(marlinPort, TCIFLUSH);

    if (tcsetattr(marlinPort, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        throw std::runtime_error("Failed to set termios");
    }

    // Start the background reader thread
    readerThread = std::thread(&MarlinController::readerThreadFunction, this);
    std::cout << "Connected to Marlin on " << port << " - background reader started" << std::endl;

    //Set Marlin to absolute positioning:
    sendGCode("G90");

    marlinState = idle;

}

MarlinController::~MarlinController() {
    disconnect();
}

void MarlinController::disconnect() {
    if (marlinPort >= 0) {
        std::cout << "Disconnecting from Marlin..." << std::endl;
        
        // Stop the reader thread first
        stopReading = true;
        
        // Give the thread a moment to see the flag and exit gracefully
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Join the thread if it's still joinable
        if (readerThread.joinable()) {
            std::cout << "Waiting for reader thread to finish..." << std::endl;
            readerThread.join();
            std::cout << "Reader thread joined successfully." << std::endl;
        }
        
        // Close the serial port
        close(marlinPort);
        marlinPort = -1;
        marlinState = disconnected;
        
        std::cout << "Marlin disconnection complete." << std::endl;
    } else {
        std::cout << "Marlin already disconnected." << std::endl;
    }
}

bool MarlinController::isConnected() const {
    return marlinPort >= 0;
}

void MarlinController::sendGCode(const std::string& gcode) {
    if (!isConnected()) {
        std::cerr << "Error: Not connected to Marlin" << std::endl;
        return;
    }
    std::string cmd = gcode + "\n";
    write(marlinPort, cmd.c_str(), cmd.size());
    std::cout << "Sent: " << gcode << std::endl;
}

void MarlinController::homeX() {

    if (!isConnected()) {
        std::cerr << "Error: Not connected to Marlin" << std::endl;
        return;
    }

    marlinState = homingX;
    sendGCode("G28 X");

}

void MarlinController::moveXTo(double position, double speed) {
       if (!isConnected()) {
        std::cerr << "Error: Not connected to Marlin" << std::endl;
        return;
    }

    marlinState = moveStarted;
    sendGCode("G0 X" + std::to_string(position) + " F" + std::to_string(speed));
    xPos = position;        //Rough POS

}

void MarlinController::homeZ() {

    if (!isConnected()) {
        std::cerr << "Error: Not connected to Marlin" << std::endl;
        return;
    }

    marlinState = homingZ;
    sendGCode("G28 Z");

}

void MarlinController::moveZTo(double position) {

    if (!isConnected()) {
        std::cerr << "Error: Not connected to Marlin" << std::endl;
        return;
    }

    marlinState = zMoveStarted;
    sendGCode("G0 Z" + std::to_string(position) + " F300");
    zPos = position;        //Rough POS
    
}

void MarlinController::setState(state newState) {
    marlinState = newState;
}

void MarlinController::extractPosition(std::string response) {
    //Parse response like:
    //X:0.00 Y:370.00 Z:0.00 E:0.00 Count X:0 Y:29600 Z:0

    size_t xPosStart = response.find("X:");     //Fuck Y
    size_t zPosStart = response.find("Z:");

    if (xPosStart != std::string::npos && zPosStart != std::string::npos) {
        try {
            xPos = std::stod(response.substr(xPosStart + 2, response.find(' ', xPosStart) - (xPosStart + 2)));
            zPos = std::stod(response.substr(zPosStart + 2, response.find(' ', zPosStart) - (zPosStart + 2)));
            //std::cout << "Extracted position: X=" << xPos << " Z=" << zPos << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Error parsing position: " << e.what() << std::endl;
        }
    }

}

void MarlinController::readerThreadFunction() {
    char buffer[1024];
    std::string lineBuffer;
    
    while (!stopReading && isConnected()) {
        int bytesRead = read(marlinPort, buffer, sizeof(buffer) - 1);
        
        if (bytesRead > 0) {
            buffer[bytesRead] = '\0';
            lineBuffer += buffer;
            
            // Process complete lines
            size_t pos = 0;
            while ((pos = lineBuffer.find('\n')) != std::string::npos) {
                std::string line = lineBuffer.substr(0, pos);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back(); // Remove carriage return
                }
                if (!line.empty()) {
                    handleResponse(line);
                }
                lineBuffer.erase(0, pos + 1);
            }
        } else {
            // Small delay to prevent busy waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    std::cout << "Reader thread stopped" << std::endl;



}

void MarlinController::handleResponse(std::string response) {

    std::cout << "MARLIN:" << response << std::endl;

    //If response starts with 'X:' interpret as positon data (after home or M114 ping)
    if (response.find("X:") == 0) {
        extractPosition(response);
        std::cout << "---------> Local position updated: X=" << xPos << " Z=" << zPos << std::endl;
        return;                         //Done (home will fall-thru to next ok)
    }

    switch(marlinState) {

        case homingX:
            //Already got position above thanks!
            if (response == "ok") {
                marlinState = xHomed;
                std::cout << "---------> X Homing complete" << std::endl;
            }

            break;

        case homingZ:
            //Already got position above thanks!
            if (response == "ok") {
                std::cout << "---------> Z Homing complete" << std::endl;
                marlinState = idle;
            }
            break;

        case zMoveStarted:    
            if (response == "ok") {
                std::cout << "---------> Z Move started & acknowledged, sending M400..." << std::endl;
                sendGCode("M400");
                marlinState = zMoveWaitForComplete1;
            }

            break;

        case zMoveWaitForComplete1:     //Not sure why extra OK happens after Z sequence HACK FIX
            if (response == "ok") {
                if (burnExtraOK) {
                    burnExtraOK = false;
                    std::cout << "---------> Z Move burning excess OK" << std::endl;
                    marlinState = zMoveWaitForComplete2;
                }
                else {
                    std::cout << "---------> Z Move complete 1" << std::endl;
                    marlinState = idle;
                }
            }

            break;

        case zMoveWaitForComplete2:

            if (response == "ok") {
                std::cout << "---------> Z Move complete 2" << std::endl;
                marlinState = idle;
            }

            break;

        case moveStarted:

            if (response == "ok") {
                std::cout << "---------> X Move started & acknowledged, sending M400..." << std::endl;
                sendGCode("M400");
                marlinState = moveWaitForComplete;
            }

            break;

        case moveWaitForComplete:

            if (response == "ok") {
                std::cout << "---------> X Move complete" << std::endl;
                marlinState = moveCompleted;
            }

            break;

        case getPosition:
            if (response == "ok") {
                std::cout << "---------> Position updated" << std::endl;
                marlinState = idle;
            }

            break;

    }


    //G0 command:
        //Returns OK
        //Then moves to wherever
        //So we send M400
        //Spams back echo:busy: processing
        //Sends OK when done

    //G28 command:
        //Does NOT return OK
        //Spams back echo:busy: processing
        //Sends position data X:0.00 Y:370.00 Z:0.00 E:0.00 Count X:0 Y:29600 Z:0
        //Sends OK

        //M114 command:
        //Sends position data X:0.00 Y:370.00 Z:0.00 E:0.00 Count X:0 Y:29600 Z:0
        //Sends OK

}

void MarlinController::setFanSpeed(int fanNumber, int speedPercent) {
    if (speedPercent < 0) speedPercent = 0;
    if (speedPercent > 100) speedPercent = 100;
    
    // Convert percentage to PWM value (0-255)
    int pwmValue = (speedPercent * 255) / 100;
    
    std::string gcode = "M106 P" + std::to_string(fanNumber) + " S" + std::to_string(pwmValue);
    std::cout << "Setting fan " << fanNumber << " to " << speedPercent << "% (" << pwmValue << " PWM)" << std::endl;
    sendGCode(gcode);
}
