#include "../include/GPIO_Pi4.h"
#include <iostream>

// Global variables for Pi 4
gpiod_chip* chip_pi4 = nullptr;
const std::chrono::milliseconds DEBOUNCE_TIME_PI4(200); // 200ms debounce

// GPIO Functions for Pi 4 (libgpiod v1.x)
bool initGPIO_Pi4(GPIOButton_Pi4& button) {
    if (!chip_pi4) {
        std::cerr << "GPIO chip not initialized" << std::endl;
        return false;
    }
    
    // Get the GPIO line (libgpiod v1.x API)
    button.line = gpiod_chip_get_line(chip_pi4, button.pin);
    if (!button.line) {
        std::cerr << "Failed to get GPIO line " << button.pin << std::endl;
        return false;
    }
    
    // Request line as input with pull-up (libgpiod v1.x API)
    if (gpiod_line_request_input_flags(button.line, "cat_feeder", GPIOD_LINE_REQUEST_FLAG_BIAS_PULL_UP) < 0) {
        std::cerr << "Failed to request GPIO line " << button.pin << " as input with pull-up" << std::endl;
        return false;
    }
    
    return true;
}

bool readGPIO_Pi4(const GPIOButton_Pi4& button) {
    if (!button.line) {
        return false;
    }
    
    int value = gpiod_line_get_value(button.line);
    if (value < 0) {
        std::cerr << "Failed to read GPIO " << button.pin << std::endl;
        return false;
    }
    
    // Return true when button is pressed (active LOW: 0 = pressed, 1 = released)
    return (value == 0);
}

void cleanupGPIO_Pi4(GPIOButton_Pi4& button) {
    if (button.line) {
        gpiod_line_release(button.line);
        button.line = nullptr;
    }
}

void initAllButtons_Pi4(std::vector<GPIOButton_Pi4>& buttons) {
    std::cout << "Initializing GPIO buttons with libgpiod v1.x (Pi 4)..." << std::endl;
    
    // Open the GPIO chip (libgpiod v1.x method)
    chip_pi4 = gpiod_chip_open_by_name("gpiochip0");
    if (!chip_pi4) {
        std::cerr << "Failed to open GPIO chip" << std::endl;
        return;
    }
    
    std::cout << "  GPIO chip opened successfully" << std::endl;
    
    for (auto& button : buttons) {
        if (initGPIO_Pi4(button)) {
            // Read initial state
            button.lastState = readGPIO_Pi4(button);
            std::cout << "  GPIO " << button.pin << " (" << button.name << ") initialized, initial state: " 
                      << (button.lastState ? "PRESSED" : "RELEASED") << std::endl;
        } else {
            std::cerr << "  Failed to initialize GPIO " << button.pin << std::endl;
        }
    }
}

void cleanupAllButtons_Pi4(std::vector<GPIOButton_Pi4>& buttons) {
    std::cout << "Cleaning up GPIO buttons..." << std::endl;
    
    for (auto& button : buttons) {
        cleanupGPIO_Pi4(button);
    }
    
    if (chip_pi4) {
        gpiod_chip_close(chip_pi4);
        chip_pi4 = nullptr;
    }
}

void checkButtons_Pi4(std::vector<GPIOButton_Pi4>& buttons) {
    auto now = std::chrono::steady_clock::now();
    
    for (auto& button : buttons) {
        if (!button.line) continue; // Skip if not initialized
        
        bool currentState = readGPIO_Pi4(button);
        
        // Check for button press (transition from released (false/HIGH) to pressed (true/LOW))
        // With pull-up: released = HIGH (false), pressed = LOW (true)  
        if (button.lastState == false && currentState == true) {
            // Check debounce time
            if (now - button.lastPress > DEBOUNCE_TIME_PI4) {
                button.lastPress = now;
                button.lastState = currentState;
                
                // Execute callback if it exists
                if (button.callback) {
                    button.callback();
                }
            }
        }
        
        button.lastState = currentState;
    }
}

// Button Management Functions for Pi 4
void setButtonCallback_Pi4(std::vector<GPIOButton_Pi4>& buttons, int pin, std::function<void()> callback) {
    for (auto& button : buttons) {
        if (button.pin == pin) {
            button.callback = callback;
            std::cout << "Updated callback for " << button.name << " (GPIO " << pin << ")" << std::endl;
            return;
        }
    }
    std::cerr << "Button with pin " << pin << " not found!" << std::endl;
}

void setButtonCallback_Pi4(std::vector<GPIOButton_Pi4>& buttons, const std::string& name, std::function<void()> callback) {
    for (auto& button : buttons) {
        if (button.name == name) {
            button.callback = callback;
            std::cout << "Updated callback for " << button.name << " (GPIO " << button.pin << ")" << std::endl;
            return;
        }
    }
    std::cerr << "Button with name '" << name << "' not found!" << std::endl;
}

void clearButtonCallback_Pi4(std::vector<GPIOButton_Pi4>& buttons, int pin) {
    setButtonCallback_Pi4(buttons, pin, nullptr);
}

void clearButtonCallback_Pi4(std::vector<GPIOButton_Pi4>& buttons, const std::string& name) {
    setButtonCallback_Pi4(buttons, name, nullptr);
}