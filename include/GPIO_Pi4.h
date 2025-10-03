#pragma once

#include <vector>
#include <functional>
#include <chrono>
#include <gpiod.h>
#include <iostream>

// GPIO Button Configuration for Pi 4 using libgpiod v1.x
// Buttons are active LOW with internal pull-up resistors
// Released state: HIGH (1), Pressed state: LOW (0)

struct GPIOButton_Pi4 {
    int pin;
    gpiod_line* line;
    bool lastState;
    std::chrono::steady_clock::time_point lastPress;
    std::string name;
    std::function<void()> callback;  // Button callback function
    
    // Constructor with callback
    GPIOButton_Pi4(int p, const std::string& n, std::function<void()> cb = nullptr) 
        : pin(p), line(nullptr), lastState(true), 
          lastPress(std::chrono::steady_clock::now()), name(n), callback(cb) {}
};

// Global GPIO chip handle for Pi 4
extern gpiod_chip* chip_pi4;
extern const std::chrono::milliseconds DEBOUNCE_TIME_PI4;

// GPIO Functions for Pi 4 (libgpiod v1.x)
bool initGPIO_Pi4(GPIOButton_Pi4& button);
bool readGPIO_Pi4(const GPIOButton_Pi4& button);
void cleanupGPIO_Pi4(GPIOButton_Pi4& button);
void initAllButtons_Pi4(std::vector<GPIOButton_Pi4>& buttons);
void cleanupAllButtons_Pi4(std::vector<GPIOButton_Pi4>& buttons);
void checkButtons_Pi4(std::vector<GPIOButton_Pi4>& buttons);

// Button management functions for Pi 4
void setButtonCallback_Pi4(std::vector<GPIOButton_Pi4>& buttons, int pin, std::function<void()> callback);
void setButtonCallback_Pi4(std::vector<GPIOButton_Pi4>& buttons, const std::string& name, std::function<void()> callback);
void clearButtonCallback_Pi4(std::vector<GPIOButton_Pi4>& buttons, int pin);
void clearButtonCallback_Pi4(std::vector<GPIOButton_Pi4>& buttons, const std::string& name);