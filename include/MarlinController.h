#pragma once
#include <string>
#include <thread>
#include <atomic>

class MarlinController {
public:

    enum state {
        disconnected,
        idle,
        homingZ,
        zMoveStarted,
        zMoveWaitForComplete1,
        zMoveWaitForComplete2,
        zMoveCompleted,
        homingX,
        xHomed,
        moveStarted,        //Sent, waiting for OK
        moveWaitForComplete,    //M400 sent, waiting for OK (move complete)
        moveCompleted,
        getPosition
    };

    state marlinState = disconnected;

    double xPos = 0.00;
    double zPos = 0.00;

    bool burnExtraOK = false;

    MarlinController(const std::string& port, int baudrate);
    ~MarlinController();

    void sendGCode(const std::string& gcode);
    void disconnect();
    bool isConnected() const;
    void handleResponse(std::string response);

    void homeX();
    void moveXTo(double position, double speed);
    void homeZ();
    void moveZTo(double position);
    void setState(state newState);
    state getState() const { return marlinState; }
    state getCurrentState() const { return marlinState; }  // Alias for consistency
    void extractPosition(std::string response);
    void setFanSpeed(int fanNumber, int speedPercent);  // Set fan speed 0-100%

private:
    int marlinPort; // File descriptor for the serial port
    std::thread readerThread;
    std::atomic<bool> stopReading;
    
    void readerThreadFunction(); // Background thread that continuously reads responses
};