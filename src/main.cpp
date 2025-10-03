#include <iostream>
#include <thread>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <functional>
#include <gpiod.h>
#include <signal.h>
#include <csignal>
#include "MarlinController.h"
#include "SSD1306.h"

enum state {
    idle,                       //Must have 1 or more cans loaded for this state
    phase1_x_homing,
    phase2_x_to_start,
    phase3_tab_lifting,
    phase4_lid_peeling,
    phase5_x_rehoming,
    phase6_z_lift_to_eject,
    phase7_x_eject,
    phase8_x_rehoming_final,
    phase9_z_next_can,
    initial_z_homing,
    initial_z_offsetting,
    loading_first,              //User forced to load at least 1 can
    canLoad_step_1,             //Moving cans down for next
    canLoad_step_2,             //Moving new loaded can for open procedure
};

state machineState = idle;
MarlinController* g_marlin = nullptr;  // Global pointer to marlin controller
SSD1306* g_display = nullptr;          // Global pointer to display
int cansLoaded = 0;
bool operationRunning = false;         // Flag to track if operation is running
bool canLoadSequence = false;
double feedGap = 8.0;                  // Feed gap in hours - can be adjusted in menu
std::time_t feedTime = 0;              // Next feeding time (Unix timestamp)

// Fan control for dispense sequence
std::time_t fanStopTime = 0;           // Time when fan should be turned off (0 = fan off)

// Schedule mode system
enum ScheduleMode {
    INTERVAL_MODE,
    DAILY_MODE
};

ScheduleMode scheduleMode = INTERVAL_MODE;
int dailyFeedHour = 6;      // 0-23 hours
int dailyFeedMinute = 30;   // 0, 15, 30, 45 minutes

// Global shutdown flag for clean exit
std::atomic<bool> g_shutdown_requested(false);

// Startup sequence flag - prevents automatic feeding during initial Z homing/positioning
bool startupSequenceComplete = false;

// Feed time checking - only check on minute boundaries
int lastFeedCheckMinute = -1;  // Track last minute we checked for feeding

// Debug output control
bool enableDebugOutput = true;   // Set to true to enable debug messages

// Signal handler for graceful shutdown
void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ". Initiating graceful shutdown..." << std::endl;
    g_shutdown_requested = true;
}

double ejectLast = 318.00;             //CAN be adjusted in the menu
double openLast = ejectLast - 21.00;
const double canToEject = 21.00;            //Move just-opened can ZUP this much so it can be ejected
const double nextCan = 37.00;               //After eject, move nextCan ZUP to bring next can level for opening
const double cartridgeHeight = 58.00;       //Z start offset = openLast - (cansLoaded * cartridgeHeight)

// GPIO Button Configuration using libgpiod with callback support
// Supports both v1.x (Pi 4) and v2.x (Pi 5) APIs
// Buttons are active LOW with internal pull-up resistors
// Released state: HIGH (1), Pressed state: LOW (0)

// GPIO Button Configuration using libgpiod v2.x (Pi 5)
// For Pi 4 compatibility, see GPIO_Pi4.h and GPIO_Pi4.cpp
// Buttons are active LOW with internal pull-up resistors
// Released state: HIGH (1), Pressed state: LOW (0)

struct GPIOButton {
    int pin;
    gpiod_line_request* request;  // v2.x API (both Pi 4 and Pi 5)
    bool lastState;
    std::chrono::steady_clock::time_point lastPress;
    std::string name;
    std::function<void()> callback;  // Button callback function
    
    // Constructor with callback
    GPIOButton(int p, const std::string& n, std::function<void()> cb = nullptr) 
        : pin(p), request(nullptr), lastState(true), 
          lastPress(std::chrono::steady_clock::now()), name(n), callback(cb) {}
};

// Forward declarations for callback functions
void buttonUpPressed();
void buttonDownPressed();
void buttonLeftPressed();
void buttonRightPressed();
void buttonOkPressed();

// Forward declarations for display functions
std::string getFeedTimeString();

// Define your buttons here - adjust pin numbers as needed
std::vector<GPIOButton> buttons = {
    GPIOButton(5, "BUTTON_UP", buttonUpPressed),
    GPIOButton(19, "BUTTON_DOWN", buttonDownPressed),
    GPIOButton(6, "BUTTON_LEFT", buttonLeftPressed),
    GPIOButton(26, "BUTTON_RIGHT", buttonRightPressed),
    GPIOButton(13, "BUTTON_OK", buttonOkPressed)
};

const auto DEBOUNCE_TIME = std::chrono::milliseconds(200); // 200ms debounce

// Global GPIO chip handle
gpiod_chip* chip = nullptr;

// GPIO Functions for libgpiod v2.x (both Pi 4 and Pi 5)
bool initGPIO(GPIOButton& button) {
    if (!chip) {
        std::cerr << "GPIO chip not initialized" << std::endl;
        return false;
    }
    
    try {
        // libgpiod v2.x API
        gpiod_line_settings* settings = gpiod_line_settings_new();
        if (!settings) {
            std::cerr << "Failed to create line settings" << std::endl;
            return false;
        }
        
        gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
        gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);
        
        gpiod_line_config* config = gpiod_line_config_new();
        if (!config) {
            gpiod_line_settings_free(settings);
            std::cerr << "Failed to create line config" << std::endl;
            return false;
        }
        
        unsigned int pin_offset = static_cast<unsigned int>(button.pin);
        gpiod_line_config_add_line_settings(config, &pin_offset, 1, settings);
        
        gpiod_request_config* req_config = gpiod_request_config_new();
        if (!req_config) {
            gpiod_line_config_free(config);
            gpiod_line_settings_free(settings);
            std::cerr << "Failed to create request config" << std::endl;
            return false;
        }
        
        gpiod_request_config_set_consumer(req_config, "cat_feeder");
        
        button.request = gpiod_chip_request_lines(chip, req_config, config);
        
        gpiod_request_config_free(req_config);
        gpiod_line_config_free(config);
        gpiod_line_settings_free(settings);
        
        if (!button.request) {
            std::cerr << "Failed to request GPIO line " << button.pin << std::endl;
            return false;
        }
        
        return true;
        
    } catch (const std::exception& e) {
        std::cerr << "Exception in initGPIO: " << e.what() << std::endl;
        return false;
    }
}

bool readGPIO(const GPIOButton& button) {
    // libgpiod v2.x API
    if (!button.request) {
        return false;
    }
    
    try {
        gpiod_line_value value = gpiod_line_request_get_value(button.request, button.pin);
        if (value == GPIOD_LINE_VALUE_ERROR) {
            std::cerr << "Failed to read GPIO " << button.pin << std::endl;
            return false;
        }
        
        return (value == GPIOD_LINE_VALUE_ACTIVE);
        
    } catch (const std::exception& e) {
        std::cerr << "Exception reading GPIO " << button.pin << ": " << e.what() << std::endl;
        return false;
    }
}

void cleanupGPIO(GPIOButton& button) {
    // libgpiod v2.x API
    if (button.request) {
        gpiod_line_request_release(button.request);
        button.request = nullptr;
    }
}

void initAllButtons() {
    std::cout << "Initializing GPIO buttons with libgpiod v2.x..." << std::endl;
    
    // Open the GPIO chip (v2.x method)
    chip = gpiod_chip_open("/dev/gpiochip0");

    if (!chip) {
        std::cerr << "Failed to open GPIO chip" << std::endl;
        return;
    }
    
    std::cout << "  GPIO chip opened successfully" << std::endl;
    
    for (auto& button : buttons) {
        if (initGPIO(button)) {
            // Read initial state
            button.lastState = readGPIO(button);
            std::cout << "  GPIO " << button.pin << " (" << button.name << ") initialized, initial state: " 
                      << (button.lastState ? "PRESSED" : "RELEASED") << std::endl;
        } else {
            std::cerr << "  Failed to initialize GPIO " << button.pin << std::endl;
        }
    }
}

void cleanupAllButtons() {
    std::cout << "Cleaning up GPIO buttons..." << std::endl;
    
    for (auto& button : buttons) {
        cleanupGPIO(button);
    }
    
    if (chip) {
        gpiod_chip_close(chip);
        chip = nullptr;
    }
}

void checkButtons() {
    auto now = std::chrono::steady_clock::now();
    
    for (auto& button : buttons) {
        if (!button.request) continue; // Skip if not initialized
        
        bool currentState = readGPIO(button);
        
        // Check for button press (transition from released (false/HIGH) to pressed (true/LOW))
        // With pull-up: released = HIGH (false), pressed = LOW (true)  
        if (button.lastState == false && currentState == true) {
            // Check debounce time
            if (now - button.lastPress > DEBOUNCE_TIME) {
                button.lastPress = now;
                button.lastState = currentState;
                //std::cout << "Button pressed: " << button.name << " (GPIO " << button.pin << ")" << std::endl;
                
                // Execute callback if it exists
                if (button.callback) {
                    button.callback();
                }
            }
        }
        
        button.lastState = currentState;
    }
}

// Button Management Functions
void setButtonCallback(int pin, std::function<void()> callback) {
    for (auto& button : buttons) {
        if (button.pin == pin) {
            button.callback = callback;
            std::cout << "Updated callback for " << button.name << " (GPIO " << pin << ")" << std::endl;
            return;
        }
    }
    std::cerr << "Button with pin " << pin << " not found!" << std::endl;
}

void setButtonCallback(const std::string& name, std::function<void()> callback) {
    for (auto& button : buttons) {
        if (button.name == name) {
            button.callback = callback;
            std::cout << "Updated callback for " << button.name << " (GPIO " << button.pin << ")" << std::endl;
            return;
        }
    }
    std::cerr << "Button with name '" << name << "' not found!" << std::endl;
}

void clearButtonCallback(int pin) {
    setButtonCallback(pin, nullptr);
}

void clearButtonCallback(const std::string& name) {
    setButtonCallback(name, nullptr);
}

// Helper function to convert machine state enum to string
std::string machineStateToString(state s) {
    switch(s) {
        case idle: return "idle";
        case phase1_x_homing: return "phase1_x_homing";
        case phase2_x_to_start: return "phase2_x_to_start";
        case phase3_tab_lifting: return "phase3_tab_lifting";
        case phase4_lid_peeling: return "phase4_lid_peeling";
        case phase5_x_rehoming: return "phase5_x_rehoming";
        case phase6_z_lift_to_eject: return "phase6_z_lift_to_eject";
        case phase7_x_eject: return "phase7_x_eject";
        case phase8_x_rehoming_final: return "phase8_x_rehoming_final";
        case phase9_z_next_can: return "phase9_z_next_can";
        case initial_z_homing: return "initial_z_homing";
        case initial_z_offsetting: return "initial_z_offsetting";
        case loading_first: return "loading_first";
        case canLoad_step_1: return "canLoad_step_1";
        case canLoad_step_2: return "canLoad_step_2";
        default: return "unknown";
    }
}

// Helper function to convert string to machine state enum
state stringToMachineState(const std::string& s) {
    if (s == "idle") return idle;
    if (s == "phase1_x_homing") return phase1_x_homing;
    if (s == "phase2_x_to_start") return phase2_x_to_start;
    if (s == "phase3_tab_lifting") return phase3_tab_lifting;
    if (s == "phase4_lid_peeling") return phase4_lid_peeling;
    if (s == "phase5_x_rehoming") return phase5_x_rehoming;
    if (s == "phase6_z_lift_to_eject") return phase6_z_lift_to_eject;
    if (s == "phase7_x_eject") return phase7_x_eject;
    if (s == "phase8_x_rehoming_final") return phase8_x_rehoming_final;
    if (s == "phase9_z_next_can") return phase9_z_next_can;
    if (s == "initial_z_homing") return initial_z_homing;
    if (s == "initial_z_offsetting") return initial_z_offsetting;
    if (s == "loading_first") return loading_first;
    if (s == "canLoad_step_1") return canLoad_step_1;
    if (s == "canLoad_step_2") return canLoad_step_2;
    return idle; // Default to idle if unknown
}

// Helper function to convert marlin state enum to string
std::string marlinStateToString(MarlinController::state s) {
    switch(s) {
        case MarlinController::disconnected: return "disconnected";
        case MarlinController::idle: return "idle";
        case MarlinController::homingZ: return "homingZ";
        //case MarlinController::zHomed: return "zHomed";
        case MarlinController::homingX: return "homingX";
        case MarlinController::xHomed: return "xHomed";
        case MarlinController::moveStarted: return "moveStarted";
        case MarlinController::moveWaitForComplete: return "moveWaitForComplete";
        case MarlinController::moveCompleted: return "moveCompleted";
        case MarlinController::getPosition: return "getPosition";
        default: return "unknown";
    }
}

// Helper function to convert string to marlin state enum
MarlinController::state stringToMarlinState(const std::string& s) {
    if (s == "disconnected") return MarlinController::disconnected;
    if (s == "idle") return MarlinController::idle;
    if (s == "homingZ") return MarlinController::homingZ;
    //if (s == "zHomed") return MarlinController::zHomed;
    if (s == "homingX") return MarlinController::homingX;
    if (s == "xHomed") return MarlinController::xHomed;
    if (s == "moveStarted") return MarlinController::moveStarted;
    if (s == "moveWaitForComplete") return MarlinController::moveWaitForComplete;
    if (s == "moveCompleted") return MarlinController::moveCompleted;
    if (s == "getPosition") return MarlinController::getPosition;
    return MarlinController::idle; // Default to idle if unknown
}

// Helper function to get the full path in user's home directory
std::string getHomeFilePath(const std::string& filename) {
    const char* home = getenv("HOME");
    
    // If running under sudo, try to get the original user's home directory
    const char* sudo_user = getenv("SUDO_USER");
    if (sudo_user != nullptr && home != nullptr && std::string(home) == "/root") {
        // We're running under sudo, construct the original user's home path
        std::string user_home = "/home/" + std::string(sudo_user);
        return user_home + "/" + filename;
    }
    
    if (home == nullptr) {
        std::cerr << "Warning: Could not get HOME environment variable, using current directory" << std::endl;
        return filename;
    }
    return std::string(home) + "/" + filename;
}

// Save machine state, marlin state, and positions to JSON file
void saveStateToJSON(const std::string& filename = "machine_state.json") {
    if (!g_marlin) {
        std::cerr << "Error: Marlin controller not initialized" << std::endl;
        return;
    }
    
    std::string fullPath = getHomeFilePath(filename);
    
    try {
        std::ofstream file(fullPath);
        if (!file.is_open()) {
            std::cerr << "Error: Could not open file " << fullPath << " for writing" << std::endl;
            return;
        }
        
        // Create a simple JSON structure
        file << "{\n";
        file << "  \"machine_state\": \"" << machineStateToString(machineState) << "\",\n";
        file << "  \"marlin_state\": \"" << marlinStateToString(g_marlin->getState()) << "\",\n";
        file << "  \"x_position\": " << g_marlin->xPos << ",\n";
        file << "  \"z_position\": " << g_marlin->zPos << ",\n";
        file << "  \"cans_loaded\": " << cansLoaded << ",\n";
        file << "  \"eject_last\": " << ejectLast << ",\n";
        file << "  \"feed_gap\": " << feedGap << ",\n";
        file << "  \"feed_time\": " << feedTime << ",\n";
        file << "  \"schedule_mode\": \"" << (scheduleMode == INTERVAL_MODE ? "INTERVAL" : "DAILY") << "\",\n";
        file << "  \"daily_feed_hour\": " << dailyFeedHour << ",\n";
        file << "  \"daily_feed_minute\": " << dailyFeedMinute << ",\n";
        file << "  \"timestamp\": \"" << std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() << "\"\n";
        file << "}\n";
        
        file.close();
        // std::cout << "State saved to " << fullPath << std::endl;
        // std::cout << "  Machine State: " << machineStateToString(machineState) << std::endl;
        // std::cout << "  Marlin State: " << marlinStateToString(g_marlin->getState()) << std::endl;
        // std::cout << "  X Position: " << g_marlin->xPos << std::endl;
        // std::cout << "  Z Position: " << g_marlin->zPos << std::endl;

         std::cout << "---> JSON STATE SAVED" << std::endl;
        
        // TEMPORARILY DISABLED - Enable this to auto-save state
        return;  // Comment out this line to re-enable auto-saving
        
        // Also save a copy for the web API (non-destructive addition)
        if (filename == "machine_state.json") {
            std::string webApiPath = getHomeFilePath("feeder_state.json");
            std::ofstream webFile(webApiPath);
            if (webFile.is_open()) {
                webFile << "{\n";
                webFile << "  \"machineState\": \"" << machineStateToString(machineState) << "\",\n";
                webFile << "  \"cansLeft\": " << cansLoaded << ",\n";
                webFile << "  \"feedMode\": \"" << (scheduleMode == INTERVAL_MODE ? "INTERVAL" : "DAILY") << "\",\n";
                webFile << "  \"feedTime\": " << feedTime << ",\n";
                webFile << "  \"feedIntervalMinutes\": " << (int)(feedGap * 60) << ",\n";
                webFile << "  \"operationRunning\": " << (operationRunning ? "true" : "false") << ",\n";
                webFile << "  \"dailyFeedHour\": " << dailyFeedHour << ",\n";
                webFile << "  \"dailyFeedMinute\": " << dailyFeedMinute << ",\n";
                webFile << "  \"timestamp\": " << std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count() << "\n";
                webFile << "}\n";
                webFile.close();
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error saving state: " << e.what() << std::endl;
    }
}

// Load machine state, marlin state, and positions from JSON file
void loadStateFromJSON(const std::string& filename = "machine_state.json") {
    if (!g_marlin) {
        std::cerr << "Error: Marlin controller not initialized" << std::endl;
        return;
    }
    
    
    std::string fullPath = getHomeFilePath(filename);
    
    try {
        std::ifstream file(fullPath);
        if (!file.is_open()) {
            std::cerr << "Error: Could not open file " << fullPath << " for reading" << std::endl;
            return;
        }
        
        std::string line;
        std::string machine_state_str, marlin_state_str;
        double x_pos = 0.0, z_pos = 0.0;
        int cans_loaded_value = 0;  // Default value
        double eject_last_value = 318.0;  // Default value
        double feed_gap_value = 8.0;  // Default value
        std::time_t feed_time_value = 0;  // Default value
        std::string schedule_mode_str = "INTERVAL";  // Default value
        int daily_feed_hour_value = 6;   // Default value
        int daily_feed_minute_value = 30; // Default value
        
        // Simple JSON parsing (very basic - assumes specific format)
        while (std::getline(file, line)) {
            // Remove whitespace
            line.erase(0, line.find_first_not_of(" \t"));
            
            if (line.find("\"machine_state\":") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t quote1 = line.find("\"", start) + 1;
                size_t quote2 = line.find("\"", quote1);
                machine_state_str = line.substr(quote1, quote2 - quote1);
            }
            else if (line.find("\"marlin_state\":") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t quote1 = line.find("\"", start) + 1;
                size_t quote2 = line.find("\"", quote1);
                marlin_state_str = line.substr(quote1, quote2 - quote1);
            }
            else if (line.find("\"x_position\":") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t comma = line.find(",", start);
                if (comma == std::string::npos) comma = line.length();
                std::string pos_str = line.substr(start, comma - start);
                pos_str.erase(0, pos_str.find_first_not_of(" \t"));
                pos_str.erase(pos_str.find_last_not_of(" \t,") + 1);
                x_pos = std::stod(pos_str);
            }
            else if (line.find("\"z_position\":") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t comma = line.find(",", start);
                if (comma == std::string::npos) comma = line.length();
                std::string pos_str = line.substr(start, comma - start);
                pos_str.erase(0, pos_str.find_first_not_of(" \t"));
                pos_str.erase(pos_str.find_last_not_of(" \t,") + 1);
                z_pos = std::stod(pos_str);
            }
            else if (line.find("\"cans_loaded\":") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t comma = line.find(",", start);
                if (comma == std::string::npos) comma = line.length();
                std::string cans_str = line.substr(start, comma - start);
                cans_str.erase(0, cans_str.find_first_not_of(" \t"));
                cans_str.erase(cans_str.find_last_not_of(" \t,") + 1);
                cans_loaded_value = std::stoi(cans_str);
            }
            else if (line.find("\"eject_last\":") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t comma = line.find(",", start);
                if (comma == std::string::npos) comma = line.length();
                std::string eject_str = line.substr(start, comma - start);
                eject_str.erase(0, eject_str.find_first_not_of(" \t"));
                eject_str.erase(eject_str.find_last_not_of(" \t,") + 1);
                eject_last_value = std::stod(eject_str);
            }
            else if (line.find("\"feed_gap\":") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t comma = line.find(",", start);
                if (comma == std::string::npos) comma = line.length();
                std::string feed_str = line.substr(start, comma - start);
                feed_str.erase(0, feed_str.find_first_not_of(" \t"));
                feed_str.erase(feed_str.find_last_not_of(" \t,") + 1);
                feed_gap_value = std::stod(feed_str);
            }
            else if (line.find("\"feed_time\":") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t comma = line.find(",", start);
                if (comma == std::string::npos) comma = line.length();
                std::string time_str = line.substr(start, comma - start);
                time_str.erase(0, time_str.find_first_not_of(" \t"));
                time_str.erase(time_str.find_last_not_of(" \t,") + 1);
                feed_time_value = std::stoll(time_str);
            }
            else if (line.find("\"schedule_mode\":") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t quote1 = line.find("\"", start) + 1;
                size_t quote2 = line.find("\"", quote1);
                schedule_mode_str = line.substr(quote1, quote2 - quote1);
            }
            else if (line.find("\"daily_feed_hour\":") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t comma = line.find(",", start);
                if (comma == std::string::npos) comma = line.length();
                std::string hour_str = line.substr(start, comma - start);
                hour_str.erase(0, hour_str.find_first_not_of(" \t"));
                hour_str.erase(hour_str.find_last_not_of(" \t,") + 1);
                daily_feed_hour_value = std::stoi(hour_str);
            }
            else if (line.find("\"daily_feed_minute\":") != std::string::npos) {
                size_t start = line.find(":") + 1;
                size_t comma = line.find(",", start);
                if (comma == std::string::npos) comma = line.length();
                std::string minute_str = line.substr(start, comma - start);
                minute_str.erase(0, minute_str.find_first_not_of(" \t"));
                minute_str.erase(minute_str.find_last_not_of(" \t,") + 1);
                daily_feed_minute_value = std::stoi(minute_str);
            }
        }
        
        file.close();
        
        // Restore the states and positions
        machineState = stringToMachineState(machine_state_str);
        g_marlin->setState(stringToMarlinState(marlin_state_str));
        g_marlin->xPos = x_pos;
        g_marlin->zPos = z_pos;
        cansLoaded = cans_loaded_value;  // Restore cansLoaded global variable
        ejectLast = eject_last_value;    // Restore ejectLast global variable
        feedGap = feed_gap_value;        // Restore feedGap global variable
        feedTime = feed_time_value;      // Restore feedTime global variable
        scheduleMode = (schedule_mode_str == "DAILY") ? DAILY_MODE : INTERVAL_MODE;
        dailyFeedHour = daily_feed_hour_value;
        dailyFeedMinute = daily_feed_minute_value;
        openLast = ejectLast - 21;       // Recalculate openLast based on loaded ejectLast
        
        std::cout << "State loaded from " << fullPath << std::endl;
        std::cout << "  Machine State: " << machine_state_str << std::endl;
        std::cout << "  Marlin State: " << marlin_state_str << std::endl;
        std::cout << "  X Position: " << x_pos << std::endl;
        std::cout << "  Z Position: " << z_pos << std::endl;
        std::cout << "  Cans Loaded: " << cans_loaded_value << std::endl;
        std::cout << "  Eject Last: " << eject_last_value << std::endl;
        std::cout << "  Feed Gap: " << feed_gap_value << " hours" << std::endl;
        std::cout << "  Feed Time: " << feed_time_value << " (timestamp)" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Error loading state: " << e.what() << std::endl;
    }
}

double setCanOpenOffset(bool sendToMarlin = false) {  //Calcs Z for opening the next can in magazine

    openLast = ejectLast - 21;      //Update this
    double offset = (openLast + cartridgeHeight) - (cansLoaded * cartridgeHeight);

    if (sendToMarlin && g_marlin) {
        g_marlin->setZPosOffsetStart(offset);
    }

    std::cout << "Can Z Offset set to: " << offset << " mm" << std::endl;

    return offset;

}

void phase1_x_homing_state(bool reset = false) {
    static bool started = false;
    
    if (reset) {
        started = false;
        return;
    }

    if (!started) {     //Starting code
        std::cout << "Entering phase 1: X Homing..." << std::endl;
        started = true;
        g_marlin->homeX();
        machineState = phase1_x_homing;     //Only need to set this on first one
        saveStateToJSON();
        return;
    }

    if (g_marlin->getState() == MarlinController::xHomed) {
        std::cout << "Phase 1 complete: X Homed" << std::endl;
        machineState = phase2_x_to_start;
        started = false;  // Reset for next time
        saveStateToJSON();
    }
}

void phase2_x_to_start_state(bool reset = false) {
    static bool started = false;
    
    if (reset) {
        started = false;
        return;
    }

    if (!started) {
        std::cout << "Entering phase 2: X to Start Position..." << std::endl;
        started = true;
        g_marlin->setState(MarlinController::moveStarted);
        g_marlin->moveXTo(165.00, 600);  // Move to X=165 at 300 mm/min
        saveStateToJSON();
        return;
    }

    if (g_marlin->getState() == MarlinController::moveCompleted) {
        std::cout << "Phase 2 complete: X to Start Position" << std::endl;
        machineState = phase3_tab_lifting;
        started = false;  // Reset for next time
        saveStateToJSON();
    }
}

void phase3_tab_lifting_state(bool reset = false) {
    static bool started = false;
    
    if (reset) {
        started = false;
        return;
    }

    if (!started) {
        std::cout << "Entering phase 3: Tab Lifting..." << std::endl;
        started = true;
        g_marlin->setState(MarlinController::moveStarted);
        g_marlin->moveXTo(248.00, 150);  // Move to X=248 at 100 mm/min
        saveStateToJSON();
        return;
    }

    if (g_marlin->getState() == MarlinController::moveCompleted) {
        std::cout << "Phase 3 complete: Tab Lifted" << std::endl;
        machineState = phase4_lid_peeling;
        started = false;  // Reset for next time
        saveStateToJSON();
    }
}

void phase4_lid_peeling_state(bool reset = false) {
    static bool started = false;
    
    if (reset) {
        started = false;
        return;
    }

    if (!started) {
        std::cout << "Entering phase 4: Lid Peeling..." << std::endl;
        started = true;
        g_marlin->setState(MarlinController::moveStarted);
        g_marlin->moveXTo(25.00, 150);  // Move to X=25 at 100 mm/min
        saveStateToJSON();
        return;
    }

    if (g_marlin->getState() == MarlinController::moveCompleted) {
        std::cout << "Phase 4 complete: Lid Peeled" << std::endl;
        machineState = phase5_x_rehoming;
        started = false;  // Reset for next time
        saveStateToJSON();
    }
}

void phase5_x_rehoming_state(bool reset = false) {
    static bool started = false;
    
    if (reset) {
        started = false;
        return;
    }

    if (!started) {     //Starting code
        std::cout << "Entering phase 5: X Re-Homing..." << std::endl;
        started = true;
        g_marlin->homeX();
        saveStateToJSON();
        return;
    }

    if (g_marlin->getState() == MarlinController::xHomed) {
        std::cout << "Phase 5 complete: X Re-Homed Like a Kitten" << std::endl;
        machineState = phase6_z_lift_to_eject;
        started = false;  // Reset for next time
        saveStateToJSON();
    }

}

void phase6_z_lift_to_eject_state(bool reset = false) {
    static bool started = false;

    if (reset) {
        started = false;
        return;
    }

    if (!started) {
        std::cout << "Entering phase 6: Z Lift to Eject Position..." << std::endl;
        started = true;
        double currentZ = g_marlin->zPos;
        currentZ += canToEject;
        g_marlin->moveZTo(currentZ);
        saveStateToJSON();
        return;
    }

    if (g_marlin->getState() == MarlinController::idle) {
        std::cout << "Phase 6 complete: Z Lifted to Eject Position" << std::endl;
        machineState = phase7_x_eject;
        started = false;  // Reset for next time
        saveStateToJSON();
    }
}

void phase7_x_eject_state(bool reset = false) {
    static bool started = false;

    if (reset) {
        started = false;
        return;
    }

    if (!started) {
        std::cout << "Entering phase 7: X Eject..." << std::endl;
        started = true;
        g_marlin->moveXTo(248, 600);  // Move to X=248 at 300 mm/min, ejects lifted can
        saveStateToJSON();
        return;
    }

    if (g_marlin->getState() == MarlinController::moveCompleted) {
        std::cout << "Phase 7 complete: X Ejected" << std::endl;
        machineState = phase8_x_rehoming_final;
        started = false;  // Reset for next time
        saveStateToJSON();
    }
}

void phase8_x_rehoming_final_state(bool reset = false) {
    static bool started = false;

    if (reset) {
        started = false;
        return;
    }

    if (!started) {
        std::cout << "Entering phase 8: X Re-Homing Final..." << std::endl;
        started = true;
        g_marlin->setState(MarlinController::moveStarted);
        g_marlin->homeX();
        saveStateToJSON();
        return;
    }

    if (g_marlin->getState() == MarlinController::xHomed) {
        std::cout << "Phase 8 complete: X Re-Homed Final" << std::endl;
        machineState = phase9_z_next_can;
        started = false;  // Reset for next time
        saveStateToJSON();
    }
}

void phase9_z_next_can_state(bool reset = false) {
    static bool started = false;

    if (reset) {
        started = false;
        return;
    }

    if (!started) {
        std::cout << "Entering phase 9: Z Next Can..." << std::endl;
        started = true;
        double currentZ = g_marlin->zPos;
        currentZ += nextCan;
        g_marlin->moveZTo(currentZ);
        saveStateToJSON();
        return;
    }

    if (g_marlin->getState() == MarlinController::idle) {
        std::cout << "Phase 9 complete: Z Next Can" << std::endl;
        std::cout << "---FEED SEQUENCE COMPLETE---" << std::endl;
        machineState = idle;
        started = false;  // Reset for next time
        cansLoaded--;
        saveStateToJSON();
    }
}

void resetAllPhases() {
    phase1_x_homing_state(true);
    phase2_x_to_start_state(true);
    phase3_tab_lifting_state(true);
    phase4_lid_peeling_state(true);
    phase5_x_rehoming_state(true);
    phase6_z_lift_to_eject_state(true);
    phase7_x_eject_state(true);
    phase8_x_rehoming_final_state(true);
    phase9_z_next_can_state(true);
}

void dispenseStateMachine() {
    switch(machineState) {
        case phase1_x_homing:
            phase1_x_homing_state();
            break;

        case phase2_x_to_start:
            phase2_x_to_start_state();
            break;

        case phase3_tab_lifting:
            phase3_tab_lifting_state();
            break;

        case phase4_lid_peeling:
            phase4_lid_peeling_state();
            break;

        case phase5_x_rehoming:
            phase5_x_rehoming_state();
            break;

        case phase6_z_lift_to_eject:
            phase6_z_lift_to_eject_state();
            break;

        case phase7_x_eject:
            phase7_x_eject_state();
            break;

        case phase8_x_rehoming_final:
            phase8_x_rehoming_final_state();
            break;

        case phase9_z_next_can:
            phase9_z_next_can_state();
            break;

        case idle:
            // Do nothing when idle
            break;
    }
}

void dispenseFoodStart() {

    if (cansLoaded < 1) {
        std::cout << "No cans loaded. Aborting dispense operation." << std::endl;
        return;
    }

    // Set operation running flag immediately to prevent double-triggering
    operationRunning = true;

    std::cout << "Starting food dispense operation..." << std::endl;
    
    // Turn on main fan at 100% for dispense operation
    if (g_marlin) {
        g_marlin->setFanSpeed(0, 100);  // Fan 0 at 100%
        g_marlin->setFanSpeed(1, 100);  // Fan 1 at 100%
    }
    
    // Reset all phase static variables
    resetAllPhases();
    
    // Reset machine state to idle
    machineState = idle;
    
    // Save initial state
    saveStateToJSON();
    
    // Start the sequence
    phase1_x_homing_state();

    //machineState = phase5_x_rehoming;  //jump test
    //phase5_x_rehoming_state();          //jump test

}

void ejectOnlyStart() {

    if (cansLoaded < 1) {
        std::cout << "No cans loaded. Aborting eject operation." << std::endl;
        return;
    }

    // Set operation running flag immediately to prevent double-triggering
    operationRunning = true;

    std::cout << "Starting eject only operation..." << std::endl;
    
    // Turn on main fan at 100% for dispense operation
    if (g_marlin) {
        g_marlin->setFanSpeed(0, 100);  // Fan 0 at 100%
        g_marlin->setFanSpeed(1, 100);  // Fan 1 at 100%
    }
    
    // Reset all phase static variables
    resetAllPhases();
    std::cout << "Jumping to phase 6..." << std::endl;
    machineState = phase6_z_lift_to_eject;
    saveStateToJSON();

}

void canLoadSequenceStart() {
    std::cout << "Starting can load sequence..." << std::endl;

    //machineState = canLoad_step_1;
    //canLoadSequence = true;

    double currentZ = g_marlin->zPos;       //Get current
    currentZ -= nextCan;
    g_marlin->moveZTo(currentZ);            // Move to new Z position

}


// =============================================================================
// BUTTON CALLBACK FUNCTIONS & MENU SYSTEM
// =============================================================================

// Menu system state
enum MenuState {
    CLOCK_SCREEN,
    MAIN_MENU,
    COMMANDS_MENU,
    SETTINGS_MENU,
    ADJUST_Z_MENU,
    LOAD_CAN_MENU,
    LOAD_CAN_INSERT_MENU,
    SCHEDULE_MODE_MENU,
    SCHEDULE_TIME_MENU,
    RUNNING_OPERATION
};

MenuState currentMenu = CLOCK_SCREEN;
int menuSelection = 0;

// Function to get current time in 12-hour format with seconds
std::string getCurrentTimeString() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto tm = *std::localtime(&time_t);
    
    std::stringstream ss;
    ss << std::put_time(&tm, "%I:%M:%S%p");
    return ss.str();
}

// Function to get current date
std::string getCurrentDateString() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto tm = *std::localtime(&time_t);
    
    std::stringstream ss;
    ss << std::put_time(&tm, "%m/%d");
    return ss.str();
}

// Function to format feed time for display
std::string getFeedTimeString() {
    if (feedTime == 0) {
        if (scheduleMode == INTERVAL_MODE) {
            return "N:Not Started";
        } else {
            return "N:Not Started";
        }
    }
    
    auto tm = *std::localtime(&feedTime);
    std::stringstream ss;
    ss << std::put_time(&tm, "%I:%M%p");
    return ss.str();
}

// Function to format feed date for display
std::string getFeedDateString() {
    if (feedTime == 0) {
        return "";
    }
    
    auto tm = *std::localtime(&feedTime);
    std::stringstream ss;
    ss << std::put_time(&tm, "%m/%d");
    return ss.str();
}

// Menu display functions
void displayClockScreen() {
    if (g_display) {
        g_display->clear();

        // Line 1: Display current time
        std::string nowDate = getCurrentDateString();
        std::string timeStr = getCurrentTimeString();
        g_display->drawString(0, 0, nowDate + " " + timeStr);

        // Line 1: Display cans left
        g_display->drawString(16, 8, "CANS LEFT: " + std::to_string(cansLoaded));

        // Line 3: Display schedule mode
        std::string modeStr = (scheduleMode == INTERVAL_MODE) ? "   -INTERVAL-" : "   -DAILY-";
        g_display->drawString(0, 16, modeStr);
        
        // Line 4: Display next feed date (if feed time is set)
        std::string feedDateStr = getFeedDateString();
        if (!feedDateStr.empty()) {
            g_display->drawString(0, 24, "NEXT:" + feedDateStr);
        }
        
        // Line 5: Display next feed time
        std::string feedStr = getFeedTimeString();
        g_display->drawString(16, 32, "AT:" + feedStr);
        
        // Line 6: Show machine state (same as before)
        std::string stateStr;
        switch (machineState) {
            case idle: stateStr = "IDLE"; break;
            case phase1_x_homing: stateStr = "HOMING"; break;
            case phase2_x_to_start: stateStr = "MOVING"; break;
            case phase3_tab_lifting: stateStr = "LIFTING"; break;
            case phase4_lid_peeling: stateStr = "PEELING"; break;
            case phase5_x_rehoming: stateStr = "REHOMING"; break;
            case initial_z_homing: stateStr = "Z INIT"; break;
            case initial_z_offsetting: stateStr = "Z SETUP"; break;
            case loading_first: stateStr = "LOADING"; break;
            case canLoad_step_1: stateStr = "LOAD1"; break;
            case canLoad_step_2: stateStr = "LOAD2"; break;
            default: stateStr = "BUSY"; break;
        }

        g_display->drawString(0, 44, ">" + stateStr);
        
        // Line 7: OK context (same as before)
        switch (machineState) {
            case idle:
                g_display->drawString(30, 56, "OK: menu");
                break;

            default:
                g_display->drawString(30, 56, "OK: abort");
                break;
        }

        g_display->display();
    }
}

void displayMainMenu() {
    if (g_display) {
        g_display->clear();
        g_display->drawString(0, 0, "MAIN MENU");
        g_display->drawString(0, 8, (menuSelection == 0 ? ">" : " ") + std::string("1.Commands"));
        g_display->drawString(0, 16, (menuSelection == 1 ? ">" : " ") + std::string("2.Settings"));
        g_display->drawString(0, 24, (menuSelection == 2 ? ">" : " ") + std::string("3.Load Can"));
        g_display->drawString(0, 48, "L:home OK:sel");
        g_display->display();
    }
}

void displayCommandsMenu() {
    if (g_display) {
        g_display->clear();
        g_display->drawString(0, 0, "COMMANDS");
        g_display->drawString(0, 8, (menuSelection == 0 ? ">" : " ") + std::string("1.Reset INT"));
        g_display->drawString(0, 16, (menuSelection == 1 ? ">" : " ") + std::string("2.Home X"));
        g_display->drawString(0, 24, (menuSelection == 2 ? ">" : " ") + std::string("3.Home Z"));
        g_display->drawString(0, 32, (menuSelection == 3 ? ">" : " ") + std::string("4.Dispense Now"));
        g_display->drawString(0, 40, (menuSelection == 4 ? ">" : " ") + std::string("5.Eject Only"));
        g_display->drawString(0, 56, "L:back OK:exe");
        g_display->display();
    }
}

void displaySettingsMenu() {
    if (g_display) {
        g_display->clear();
        g_display->drawString(0, 0, "SETTINGS");
        g_display->drawString(0, 8, (menuSelection == 0 ? ">" : " ") + std::string("1.Cans:") + std::to_string(cansLoaded));
        g_display->drawString(0, 16, (menuSelection == 1 ? ">" : " ") + std::string("2.Adjust Z"));
        
        // Option 3: Schedule mode toggle
        std::string scheduleText = (scheduleMode == INTERVAL_MODE) ? "Interval" : "Daily";
        g_display->drawString(0, 24, (menuSelection == 2 ? ">" : " ") + std::string("3.Feed:") + scheduleText);
        
        // Option 4: Show the time/interval setting based on mode
        if (scheduleMode == INTERVAL_MODE) {
            g_display->drawString(0, 32, (menuSelection == 3 ? ">" : " ") + std::string("4.Gap:") + std::to_string((int)feedGap) + "h");
        } else {
            // Format time as HH:MM
            std::string timeStr = (dailyFeedHour < 10 ? "0" : "") + std::to_string(dailyFeedHour) + 
                                 ":" + (dailyFeedMinute < 10 ? "0" : "") + std::to_string(dailyFeedMinute);
            g_display->drawString(0, 32, (menuSelection == 3 ? ">" : " ") + std::string("4.Time:") + timeStr);
        }
        
        g_display->drawString(0, 48, "L:back OK:set");
        g_display->display();
    }
}

void displayAdjustZMenu() {
    if (g_display) {
        g_display->clear();
        g_display->drawString(0, 0, "ADJUST Z");
        g_display->drawString(0, 8, "EjectLast:");
        g_display->drawString(0, 16, std::to_string(ejectLast) + " mm");
        g_display->drawString(0, 24, "OpenLast:");
        g_display->drawString(0, 32, std::to_string(openLast) + " mm");
        g_display->drawString(0, 48, "L:back U/D:adj");
        g_display->display();
    }
}

void displayScheduleModeMenu() {
    if (g_display) {
        g_display->clear();
        g_display->drawString(0, 0, "SCHEDULE MODE");
        g_display->drawString(0, 16, (menuSelection == 0 ? ">" : " ") + std::string("Interval"));
        g_display->drawString(0, 24, (menuSelection == 1 ? ">" : " ") + std::string("Daily"));
        g_display->drawString(0, 48, "L:back OK:set");
        g_display->display();
    }
}

void displayScheduleTimeMenu() {
    if (g_display) {
        g_display->clear();
        
        if (scheduleMode == INTERVAL_MODE) {
            g_display->drawString(0, 0, "FEED INTERVAL");
            g_display->drawString(0, 16, "Hours: " + std::to_string((int)feedGap));
            g_display->drawString(0, 56, "L:back U/D:adj");
        } else {
            g_display->drawString(0, 0, "DAILY FEED TIME");
            g_display->drawString(0, 8, (menuSelection == 0 ? ">" : " ") + std::string("Hour: ") + 
                                 (dailyFeedHour < 10 ? "0" : "") + std::to_string(dailyFeedHour));
            g_display->drawString(0, 16, (menuSelection == 1 ? ">" : " ") + std::string("Min: ") + 
                                 (dailyFeedMinute < 10 ? "0" : "") + std::to_string(dailyFeedMinute));
            g_display->drawString(0, 40, "U/D:adj OK:toggl");
            g_display->drawString(0, 56, "L:back");
        }
        
        g_display->display();
    }
}

// LOAD CAN Menu - First level: Move existing cans down
void displayLoadCanMenu() {
    if (g_display) {
        g_display->clear();
        g_display->drawString(0, 0, "CAN LOADING");
        g_display->drawString(0, 16, "--STEP 1");
        g_display->drawString(0, 24, "  MOVING");
        g_display->drawString(0, 32, "  CANS DOWN");
        g_display->drawString(0, 48, "L:back OK:move");
        g_display->display();
    }
}

// LOAD CAN Menu - Second level: Lower inserted can
void displayLoadCanInsertMenu() {
    if (g_display) {
        g_display->clear();
        g_display->drawString(0, 0, "CAN LOADING");
        g_display->drawString(0, 16, "--STEP 2");
        g_display->drawString(0, 24, "  LOAD NEW CAN");
        g_display->drawString(0, 32, "  OK WHEN DONE");
        g_display->drawString(0, 48, "L:back OK:done");
        g_display->display();
    }
}

void displayStatus() {
    if (g_display) {
        g_display->clear();
        g_display->drawString(0, 0, "STATUS");
        
        // Show machine state
        std::string stateStr;
        switch (machineState) {
            case idle: stateStr = "Idle"; break;
            case phase1_x_homing: stateStr = "Homing X"; break;
            case phase2_x_to_start: stateStr = "Move Start"; break;
            case phase3_tab_lifting: stateStr = "Tab Lifting"; break;
            case phase4_lid_peeling: stateStr = "Lid Peeling"; break;
            case phase5_x_rehoming: stateStr = "Rehoming"; break;
            case initial_z_homing: stateStr = "Init Z Home"; break;
            case initial_z_offsetting: stateStr = "Z Offset"; break;
            case loading_first: stateStr = "Load First"; break;
            case canLoad_step_1: stateStr = "Load Move"; break;
            case canLoad_step_2: stateStr = "Load Level"; break;
            default: stateStr = "Unknown"; break;
        }
        g_display->drawString(0, 8, "State:" + stateStr);
        
        // Show cans loaded
        g_display->drawString(0, 16, "Cans:" + std::to_string(cansLoaded));
        
        // Show Marlin state if available
        if (g_marlin) {
            std::string marlinStr;
            switch (g_marlin->marlinState) {
                case MarlinController::disconnected: marlinStr = "Disconn"; break;
                case MarlinController::idle: marlinStr = "Ready"; break;
                case MarlinController::moveStarted: marlinStr = "Moving"; break;
                default: marlinStr = "Unknown"; break;
            }
            g_display->drawString(0, 24, "Marlin:" + marlinStr);
        }
        
        g_display->drawString(0, 48, "Press any key");
        g_display->display();
    }
}

void abortOperation() {
    if (operationRunning) {
        std::cout << "Aborting operation..." << std::endl;
        
        // Send emergency stop to Marlin first
        if (g_marlin) {
            std::cout << "Sending emergency stop to Marlin..." << std::endl;
            g_marlin->sendGCode("M112");  // Emergency stop - immediate halt
            // Also turn off fans
            g_marlin->setFanSpeed(0, 0);
            g_marlin->setFanSpeed(1, 0);
        }
        
        operationRunning = false;
        
        // Reset all phases
        resetAllPhases();
        
        // Reset states
        machineState = idle;
        if (g_marlin) {
            g_marlin->setState(MarlinController::idle);
        }
        
        // Save aborted state
        saveStateToJSON();
        
        // Return to clock screen
        currentMenu = CLOCK_SCREEN;
        displayClockScreen();
        
        std::cout << "Operation aborted." << std::endl;
    }
}

// Check for web commands (non-destructive addition)
void checkWebCommands() {
    static std::time_t lastCommandCheck = 0;
    
    // Only check every 2 seconds to avoid excessive file I/O
    auto now = std::chrono::system_clock::now();
    std::time_t currentTime = std::chrono::system_clock::to_time_t(now);
    
    if (currentTime - lastCommandCheck < 2) {
        return;
    }
    lastCommandCheck = currentTime;
    
    std::string commandFile = getHomeFilePath("web_commands.json");
    
    // Check if command file exists
    std::ifstream file(commandFile);
    if (!file.is_open()) {
        return; // No commands waiting
    }
    
    try {
        std::string line;
        std::string jsonContent;
        while (std::getline(file, line)) {
            jsonContent += line;
        }
        file.close();
        
        // Simple JSON parsing for action field
        size_t actionPos = jsonContent.find("\"action\":");
        if (actionPos == std::string::npos) {
            return;
        }
        
        size_t startQuote = jsonContent.find("\"", actionPos + 9);
        size_t endQuote = jsonContent.find("\"", startQuote + 1);
        if (startQuote == std::string::npos || endQuote == std::string::npos) {
            return;
        }
        
        std::string action = jsonContent.substr(startQuote + 1, endQuote - startQuote - 1);
        
        // Process the command
        if (action == "manual_feed") {
            std::cout << "Web API: Manual feed command received" << std::endl;
            if (!operationRunning && machineState == idle && startupSequenceComplete) {
                std::cout << "Web API: Starting manual feed..." << std::endl;
                dispenseFoodStart(); // Manual feed
            } else {
                std::cout << "Web API: Cannot start manual feed - machine busy or not ready" << std::endl;
            }
        }
        
        // Delete the command file after processing
        std::remove(commandFile.c_str());
        
    } catch (const std::exception& e) {
        std::cout << "Web API: Error processing command: " << e.what() << std::endl;
        // Try to remove the file anyway
        std::remove(commandFile.c_str());
    }
}

// Default button callback functions
void buttonUpPressed() {
    switch(currentMenu) {
        case CLOCK_SCREEN:
            // No navigation on clock screen
            break;
        case MAIN_MENU:
            menuSelection = (menuSelection > 0) ? menuSelection - 1 : 2;
            displayMainMenu();
            break;
        case COMMANDS_MENU:
            menuSelection = (menuSelection > 0) ? menuSelection - 1 : 4;
            displayCommandsMenu();
            break;
        case SETTINGS_MENU:
            menuSelection = (menuSelection > 0) ? menuSelection - 1 : 3;
            displaySettingsMenu();
            break;
        case LOAD_CAN_MENU:
            // No up/down navigation needed
            break;
        case LOAD_CAN_INSERT_MENU:
            // No up/down navigation needed
            break;
        case ADJUST_Z_MENU:
            // Increase ejectLast by 0.25mm
            ejectLast += 0.25;
            if (g_marlin) {
                g_marlin->moveZTo(setCanOpenOffset());  // Move to new position
            }
            displayAdjustZMenu();
            break;
        case SCHEDULE_MODE_MENU:
            menuSelection = (menuSelection > 0) ? menuSelection - 1 : 1;
            displayScheduleModeMenu();
            break;
        case SCHEDULE_TIME_MENU:
            if (scheduleMode == INTERVAL_MODE) {
                // Increase feed gap by 1 hour
                feedGap += 1.0;
                if (feedGap > 48.0) feedGap = 48.0; // Max 48 hours
                displayScheduleTimeMenu();
            } else {
                // Daily mode - adjust hour/minute
                if (menuSelection == 0) {
                    // Adjust hour
                    dailyFeedHour = (dailyFeedHour + 1) % 24;
                } else {
                    // Adjust minute in 15-minute increments
                    dailyFeedMinute += 1;
                    if (dailyFeedMinute >= 60) dailyFeedMinute = 0;
                }
                displayScheduleTimeMenu();
            }
            break;
        case RUNNING_OPERATION:
            std::cout << "Operation running... UP button pressed" << std::endl;
            break;
    }
}

void buttonDownPressed() {
    switch(currentMenu) {
        case CLOCK_SCREEN:
            // No navigation on clock screen
            break;
        case MAIN_MENU:
            menuSelection = (menuSelection < 2) ? menuSelection + 1 : 0;
            displayMainMenu();
            break;
        case COMMANDS_MENU:
            menuSelection = (menuSelection < 4) ? menuSelection + 1 : 0;
            displayCommandsMenu();
            break;
        case SETTINGS_MENU:
            menuSelection = (menuSelection < 3) ? menuSelection + 1 : 0;
            displaySettingsMenu();
            break;
        case LOAD_CAN_MENU:
            // No up/down navigation needed
            break;
        case LOAD_CAN_INSERT_MENU:
            // No up/down navigation needed
            break;
        case ADJUST_Z_MENU:
            // Decrease ejectLast by 0.25mm
            ejectLast -= 0.25;
            if (g_marlin) {
                g_marlin->moveZTo(setCanOpenOffset());  // Move to new position
            }
            displayAdjustZMenu();
            break;
        case SCHEDULE_MODE_MENU:
            menuSelection = (menuSelection < 1) ? menuSelection + 1 : 0;
            displayScheduleModeMenu();
            break;
        case SCHEDULE_TIME_MENU:
            if (scheduleMode == INTERVAL_MODE) {
                // Decrease feed gap by 1 hour
                feedGap -= 1.0;
                if (feedGap < 1.0) feedGap = 1.0; // Min 1 hour
                displayScheduleTimeMenu();
            } else {
                // Daily mode - adjust hour/minute
                if (menuSelection == 0) {
                    // Adjust hour
                    dailyFeedHour = (dailyFeedHour - 1 + 24) % 24;
                } else {
                    // Adjust minute in 15-minute increments
                    dailyFeedMinute -= 1;
                    if (dailyFeedMinute < 0) dailyFeedMinute = 59;
                }
                displayScheduleTimeMenu();
            }
            break;
        case RUNNING_OPERATION:
            std::cout << "Operation running... DOWN button pressed" << std::endl;
            break;
    }
}

void buttonLeftPressed() {
    switch(currentMenu) {
        case CLOCK_SCREEN:
            // No action on clock screen
            break;
        case MAIN_MENU:
            currentMenu = CLOCK_SCREEN;
            displayClockScreen();
            break;
        case COMMANDS_MENU:
        case SETTINGS_MENU:
            // If leaving settings and in daily mode, automatically activate daily scheduling
            if (currentMenu == SETTINGS_MENU && scheduleMode == DAILY_MODE) {
                auto now = std::time(nullptr);
                struct tm* timeInfo = std::localtime(&now);
                timeInfo->tm_hour = dailyFeedHour;
                timeInfo->tm_min = dailyFeedMinute;
                timeInfo->tm_sec = 0;
                
                std::time_t todayFeedTime = std::mktime(timeInfo);
                
                // Smart scheduling: future time today, past time tomorrow
                if (todayFeedTime < now) {
                    // Time has passed today - schedule for tomorrow
                    todayFeedTime += 24 * 3600; // Add 24 hours
                    std::cout << "Daily mode: Time is in the past, scheduling for tomorrow" << std::endl;
                } else {
                    // Time is in the future today - schedule for today (great for testing!)
                    std::cout << "Daily mode: Time is in the future, scheduling for today" << std::endl;
                }
                
                feedTime = todayFeedTime;
                
                char timeStr[32];
                struct tm* feedTm = std::localtime(&feedTime);
                std::strftime(timeStr, sizeof(timeStr), "%I:%M %p", feedTm);
                
                std::cout << "Daily mode activated. Next feed at " << timeStr << std::endl;
            }
            
            currentMenu = MAIN_MENU;
            menuSelection = 0;
            saveStateToJSON();          //If cans changed
            displayMainMenu();
            break;
        case LOAD_CAN_MENU:
            currentMenu = MAIN_MENU;
            menuSelection = 0;
            displayMainMenu();
            break;
        case LOAD_CAN_INSERT_MENU:
            currentMenu = LOAD_CAN_MENU;
            menuSelection = 0;
            displayLoadCanMenu();
            break;
        case ADJUST_Z_MENU:
            currentMenu = SETTINGS_MENU;
            menuSelection = 0;
            saveStateToJSON();          // Save any Z adjustments
            setCanOpenOffset(true);     // Update the Z offset calculation
            displaySettingsMenu();
            break;
        case SCHEDULE_MODE_MENU:
            currentMenu = SETTINGS_MENU;
            menuSelection = 2;
            displaySettingsMenu();
            break;
        case SCHEDULE_TIME_MENU:
            currentMenu = SETTINGS_MENU;
            menuSelection = 3;
            saveStateToJSON();          // Save schedule settings
            displaySettingsMenu();
            break;
        case RUNNING_OPERATION:
            std::cout << "Returning to main menu..." << std::endl;
            currentMenu = MAIN_MENU;
            menuSelection = 0;
            displayMainMenu();
            break;
    }
}

void buttonRightPressed() {
    std::cout << "RIGHT button - not used in menu navigation" << std::endl;
}

void buttonOkPressed() {
    switch(currentMenu) {
        case CLOCK_SCREEN:
            if (operationRunning) {
                abortOperation();
            } else {
                currentMenu = MAIN_MENU;
                menuSelection = 0;
                displayMainMenu();
            }
            break;
        case MAIN_MENU:
            switch(menuSelection) {
                case 0: // Commands
                    currentMenu = COMMANDS_MENU;
                    menuSelection = 0;
                    displayCommandsMenu();
                    break;
                case 1: // Settings
                    currentMenu = SETTINGS_MENU;
                    menuSelection = 0;
                    displaySettingsMenu();
                    break;
                case 2: // Load Can
                    if (cansLoaded < 6) {
                        canLoadSequence = true;
                        machineState = canLoad_step_1;
                        currentMenu = LOAD_CAN_MENU;
                        menuSelection = 0;
                        displayLoadCanMenu();
                    }

                    break;
            }
            break;
            
        case COMMANDS_MENU:
            switch(menuSelection) {
                case 0: { // Reset INT
                    auto now = std::time(nullptr);
                    
                    // Automatically switch to interval mode
                    scheduleMode = INTERVAL_MODE;
                    
                    // Reset interval timer to feedGap hours from now
                    feedTime = now + (feedGap * 3600);
                    std::cout << "Switched to interval mode. Next feed in " << feedGap << " hours." << std::endl;
                    
                    saveStateToJSON(); // Save the updated state
                    
                    // Jump directly back to clock screen
                    currentMenu = CLOCK_SCREEN;
                    displayClockScreen();
                    break;
                }
                case 1: // Home X
                    std::cout << "Executing: Home X" << std::endl;
                    if (g_marlin) g_marlin->homeX();
                    break;
                case 2: // Home Z
                    std::cout << "Executing: Home Z" << std::endl;
                    setCanOpenOffset(true);     //Update Marlin with latest can count
                    if (g_marlin) g_marlin->homeZ();
                    break;
                case 3: // Food Dispense
                    if (cansLoaded > 0) {
                        std::cout << "Starting Food Dispense..." << std::endl;
                        currentMenu = RUNNING_OPERATION;
                        dispenseFoodStart();  // Manual dispense - don't advance schedule
                        // Don't change menu back immediately - let the main loop handle completion
                    }
                    break;
                case 4: // Eject Only
                    if (cansLoaded > 0) {
                        std::cout << "Starting Eject Only..." << std::endl;
                        currentMenu = RUNNING_OPERATION;
                        ejectOnlyStart();  // Eject can without opening
                        // Don't change menu back immediately - let the main loop handle completion
                    }
                    break;
            }
            break;
            
        case SETTINGS_MENU:
            switch(menuSelection) {
                case 0: // Cans Loaded
                    cansLoaded = (cansLoaded < 6) ? cansLoaded + 1 : 0;
                    std::cout << "Cans loaded set to: " << cansLoaded << std::endl;
                    displaySettingsMenu();
                    break;
                case 1: // Adjust Z
                    currentMenu = ADJUST_Z_MENU;
                    displayAdjustZMenu();
                    break;
                case 2: // Schedule Mode
                    currentMenu = SCHEDULE_MODE_MENU;
                    menuSelection = (scheduleMode == INTERVAL_MODE) ? 0 : 1;
                    displayScheduleModeMenu();
                    break;
                case 3: // Time/Interval Setting
                    currentMenu = SCHEDULE_TIME_MENU;
                    if (scheduleMode == DAILY_MODE) {
                        menuSelection = 0; // Start with hour selection
                    } else {
                        menuSelection = 0; // No sub-selection for interval mode
                    }
                    displayScheduleTimeMenu();
                    break;
            }
            break;
            
        case ADJUST_Z_MENU:
            // OK button in Adjust Z menu - no specific action, just stay in menu
            std::cout << "Adjust Z menu - use UP/DOWN to change, LEFT to go back" << std::endl;
            break;
            
        case SCHEDULE_MODE_MENU:
            // Set the schedule mode based on selection
            scheduleMode = (menuSelection == 0) ? INTERVAL_MODE : DAILY_MODE;
            std::cout << "Schedule mode set to: " << (scheduleMode == INTERVAL_MODE ? "Interval" : "Daily") << std::endl;
            
            currentMenu = SETTINGS_MENU;
            menuSelection = 2;
            saveStateToJSON(); // Save the schedule mode change
            displaySettingsMenu();
            break;
            
        case SCHEDULE_TIME_MENU:
            if (scheduleMode == INTERVAL_MODE) {
                // Just go back to settings - interval was adjusted with UP/DOWN
                currentMenu = SETTINGS_MENU;
                menuSelection = 3;
                saveStateToJSON();
                displaySettingsMenu();
            } else {
                // Daily mode - toggle between hour and minute selection
                if (menuSelection == 0) {
                    menuSelection = 1; // Switch to minute selection
                } else {
                    menuSelection = 0; // Switch back to hour selection
                }
                displayScheduleTimeMenu();
            }
            break;
            
        case LOAD_CAN_MENU:
            if (cansLoaded < 6 && g_marlin->getCurrentState() == MarlinController::idle) {
                canLoadSequenceStart();
                // Automatically proceed to next menu after completion
                currentMenu = LOAD_CAN_INSERT_MENU;
                menuSelection = 0;
                displayLoadCanInsertMenu();     //Next menu
            }
            break;
            
        case LOAD_CAN_INSERT_MENU:
            if (g_marlin->getCurrentState() == MarlinController::idle) {    //Wait for step
                cansLoaded += 1;        //Inc this
                g_marlin->moveZTo(setCanOpenOffset());  // Move new can to next open z height
                currentMenu = MAIN_MENU;
                menuSelection = 2;      // Position cursor on "3.Load Can" for easy repeated loading
                displayMainMenu();
            }
            break;
            
        case RUNNING_OPERATION:
            std::cout << "OK pressed during operation" << std::endl;
            break;
    }
}

// Function to switch to different button contexts
void setMenuContext() {
    // Reset to default menu callbacks
    setButtonCallback("BUTTON_UP", buttonUpPressed);
    setButtonCallback("BUTTON_DOWN", buttonDownPressed);
    setButtonCallback("BUTTON_LEFT", buttonLeftPressed);
    setButtonCallback("BUTTON_RIGHT", buttonRightPressed);
    setButtonCallback("BUTTON_OK", buttonOkPressed);
    std::cout << "Menu context active" << std::endl;
}

void setOperationContext() {
    currentMenu = RUNNING_OPERATION;
    
    // Set different callbacks for operation mode
    setButtonCallback("BUTTON_UP", []() { 
        std::cout << "Operation: Emergency stop!" << std::endl; 
    });
    setButtonCallback("BUTTON_DOWN", []() { 
        std::cout << "Operation: Pause" << std::endl; 
    });
    setButtonCallback("BUTTON_LEFT", []() { 
        std::cout << "Operation: Cancel - returning to menu" << std::endl;
        setMenuContext();
        currentMenu = MAIN_MENU;
        menuSelection = 0;
        displayMainMenu();
    });
    setButtonCallback("BUTTON_RIGHT", []() { 
        std::cout << "Operation: Resume" << std::endl; 
    });
    setButtonCallback("BUTTON_OK", []() { 
        std::cout << "Operation: Status check" << std::endl; 
    });
    
    std::cout << "Operation context active - buttons now control operation" << std::endl;
}

// Example: Custom context for debugging/testing
void setDebugContext() {
    setButtonCallback("BUTTON_UP", []() { 
        std::cout << "DEBUG: Increment test value" << std::endl; 
    });
    setButtonCallback("BUTTON_DOWN", []() { 
        std::cout << "DEBUG: Decrement test value" << std::endl; 
    });
    setButtonCallback("BUTTON_LEFT", []() { 
        std::cout << "DEBUG: Previous test" << std::endl; 
    });
    setButtonCallback("BUTTON_RIGHT", []() { 
        std::cout << "DEBUG: Next test" << std::endl; 
    });
    setButtonCallback("BUTTON_OK", []() { 
        std::cout << "DEBUG: Execute test - returning to menu" << std::endl;
        setMenuContext();
        currentMenu = MAIN_MENU;
        menuSelection = 0;
        displayMainMenu();
    });
    
    std::cout << "Debug context active - buttons now control debug functions" << std::endl;
}

int main() {
    std::cout << "Cat Feeder Control System" << std::endl;
    std::cout << "=========================" << std::endl;

    // Register signal handlers for graceful shutdown
    signal(SIGINT, signalHandler);   // Ctrl+C
    signal(SIGTERM, signalHandler);  // Termination request

    try {
        MarlinController marlin("/dev/ttyACM0", 115200); // Adjust port as needed
        g_marlin = &marlin;  // Set global pointer

        // Initialize display
        SSD1306 display(0x3C);  // Initialize with I2C address 0x3C
        g_display = &display;   // Set global pointer
        
        if (g_display->init()) {
            g_display->drawString(0, 0, "Cat Feeder");
            g_display->drawString(0, 16, "Initializing...");
            g_display->display();
            std::cout << "Display initialized successfully" << std::endl;
        } else {
            std::cout << "Warning: Display initialization failed" << std::endl;
        }

        // Initialize GPIO buttons
        initAllButtons();

        loadStateFromJSON();        //Get this (z offset and can count)

        // Safety check: If loaded feed time is in the past, reschedule appropriately
        if (feedTime > 0) {
            auto now = std::time(nullptr);
            if (feedTime < now) {  // Only reschedule if actually in the past, not equal
                std::cout << "*** WARNING: Loaded feed time is in the past! Rescheduling... ***" << std::endl;
                
                if (scheduleMode == DAILY_MODE) {
                    // Daily mode: set to tomorrow at the specified time
                    struct tm* timeInfo = std::localtime(&now);
                    timeInfo->tm_hour = dailyFeedHour;
                    timeInfo->tm_min = dailyFeedMinute;
                    timeInfo->tm_sec = 0;
                    timeInfo->tm_mday += 1; // Tomorrow
                    
                    feedTime = std::mktime(timeInfo);
                    
                    char timeStr[32];
                    struct tm* feedTm = std::localtime(&feedTime);
                    std::strftime(timeStr, sizeof(timeStr), "%I:%M %p", feedTm);
                    std::cout << "Daily mode: Rescheduled to tomorrow at " << timeStr << std::endl;
                    
                } else {
                    // Interval mode: set to current time + feedGap hours
                    feedTime = now + (feedGap * 3600);
                    std::cout << "Interval mode: Rescheduled to " << feedGap << " hours from now" << std::endl;
                }
                
                saveStateToJSON(); // Save the corrected feed time
            }
        }

        // If we loaded daily mode and no feed time is set, automatically activate daily scheduling
        if (scheduleMode == DAILY_MODE && feedTime == 0) {
            auto now = std::time(nullptr);
            struct tm* timeInfo = std::localtime(&now);
            
            std::cout << "=== DAILY MODE AUTO-ACTIVATION ===" << std::endl;
            std::cout << "Current time: " << timeInfo->tm_hour << ":" << timeInfo->tm_min << " on " 
                      << timeInfo->tm_year+1900 << "/" << timeInfo->tm_mon+1 << "/" << timeInfo->tm_mday << std::endl;
            std::cout << "Setting daily feed to: " << dailyFeedHour << ":" << dailyFeedMinute << std::endl;
            
            timeInfo->tm_hour = dailyFeedHour;
            timeInfo->tm_min = dailyFeedMinute;
            timeInfo->tm_sec = 0;
            
            std::time_t todayFeedTime = std::mktime(timeInfo);
            
            std::cout << "Today's feed time would be: " << todayFeedTime << std::endl;
            std::cout << "Current time timestamp: " << now << std::endl;
            std::cout << "Difference: " << (todayFeedTime - now) << " seconds" << std::endl;
            
            // If the time has already passed today, schedule for tomorrow
            if (todayFeedTime < now) {  // Only reschedule if actually in the past, not equal
                todayFeedTime += 24 * 3600; // Add 24 hours
                std::cout << "Time is in past - scheduling for tomorrow" << std::endl;
            } else {
                std::cout << "Time is in future - scheduling for today" << std::endl;
            }
            
            feedTime = todayFeedTime;
            
            char timeStr[32];
            struct tm* feedTm = std::localtime(&feedTime);
            std::strftime(timeStr, sizeof(timeStr), "%I:%M %p", feedTm);
            
            std::cout << "Daily mode auto-activated. Next feed at " << timeStr << std::endl;
            std::cout << "Final feedTime timestamp: " << feedTime << std::endl;
            std::cout << "=================================" << std::endl;
            saveStateToJSON(); // Save the activated daily schedule
        }

      //TODO CHECK FOR OFF STATE HERE

        setCanOpenOffset(true);     //Update Marlin with that data
        
        // Set machine state to track Z homing during startup
        machineState = initial_z_homing;
        g_marlin->homeZ();          //Knowing that, do this Z home and will then offset so next can is in load position
        // saveStateToJSON();          // TEMPORARILY DISABLED - Save the startup state

        // Display initial clock screen
        std::cout << "\n=== SYSTEM READY ===" << std::endl;
        displayClockScreen();
        
        // Wait a moment for display to be visible
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        bool active = true;
        auto lastClockUpdate = std::chrono::steady_clock::now();

        while(active && !g_shutdown_requested) {
            // Check for GPIO button presses (callbacks handle the logic)
            checkButtons();
            
            // Handle startup Z homing sequence
            if (machineState == initial_z_homing && g_marlin && g_marlin->getCurrentState() == MarlinController::idle) {
                std::cout << "Startup Z homing complete - machine now idle" << std::endl;
                machineState = idle;
                saveStateToJSON();
            }
            
            // Check if startup sequence is complete (Z homing and positioning finished)
            if (!startupSequenceComplete && g_marlin && g_marlin->getCurrentState() == MarlinController::idle && machineState == idle) {
                startupSequenceComplete = true;
                std::cout << "*** STARTUP SEQUENCE COMPLETE - Automatic feeding now enabled ***" << std::endl;
            }
            
            // Check if it's time to start a scheduled feed
            if (!operationRunning && machineState == idle && feedTime > 0 && startupSequenceComplete) {
                auto now = std::chrono::system_clock::now();
                std::time_t currentTime = std::chrono::system_clock::to_time_t(now);
                
                if (currentTime >= feedTime) {
                    std::cout << "*** DEBUG: Time to feed detected! (current: " << currentTime 
                              << ", feed: " << feedTime << ") ***" << std::endl;
                    
                    // Advance feed time to prevent repeated triggering
                    if (scheduleMode == DAILY_MODE) {
                        feedTime += 24 * 3600; // Add 24 hours for tomorrow
                        std::cout << "*** DEBUG: Advanced daily feed time to tomorrow ***" << std::endl;
                    } else {
                        feedTime = currentTime + (feedGap * 3600); // Add interval
                        std::cout << "*** DEBUG: Advanced interval feed time by " << feedGap << " hours ***" << std::endl;
                    }
                    saveStateToJSON(); // Save the new feed time
                    
                    // TODO: Start feeding routine here
                    dispenseFoodStart();  // Feed time already advanced above

                }
            }
            
            // Update operation if running
            if (operationRunning) {
                dispenseStateMachine();
                
                // Check if operation is complete
                if (g_marlin->getCurrentState() == MarlinController::idle && machineState == idle) {
                    operationRunning = false;
                    std::cout << "Food dispense operation complete!" << std::endl;
                    
                    // Set fan to continue running for 5 more minutes
                    auto now = std::chrono::system_clock::now();
                    fanStopTime = std::chrono::system_clock::to_time_t(now + std::chrono::minutes(5));
                    std::cout << "Fan will continue running for 5 minutes..." << std::endl;
                    
                    // Save completion state
                    saveStateToJSON();
                    
                    // Return to main menu if we were in operation context
                    if (currentMenu == RUNNING_OPERATION) {
                        currentMenu = MAIN_MENU;
                        menuSelection = 0;
                        displayMainMenu();
                    }
                }
            }
            
            // Check for web commands (non-destructive addition)
            checkWebCommands();
            
            // Check if fan should be turned off
            if (fanStopTime > 0) {
                auto now = std::chrono::system_clock::now();
                std::time_t currentTime = std::chrono::system_clock::to_time_t(now);
                
                if (currentTime >= fanStopTime) {
                    std::cout << "Turning off fans after cooldown period" << std::endl;
                    if (g_marlin) {
                        g_marlin->setFanSpeed(0, 0);  // Turn off fan 0
                        g_marlin->setFanSpeed(1, 0);  // Turn off fan 1
                    }
                    fanStopTime = 0;  // Reset timer
                }
            }

            // Update clock display every second when on clock screen
            if (currentMenu == CLOCK_SCREEN) {
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastClockUpdate).count() >= 1000) {
                    displayClockScreen();
                    lastClockUpdate = now;
                }
            }
            
            // Small delay to prevent busy waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        
        // Check if shutdown was requested
        if (g_shutdown_requested) {
            std::cout << "Shutdown requested. Cleaning up..." << std::endl;
            
            // Save current state before shutdown
            if (g_marlin) {
                saveStateToJSON();
            }
        }
        
        // Cleanup GPIO before exit
        cleanupAllButtons();
        
        g_marlin = nullptr;  // Clear global pointer - this will trigger MarlinController destructor
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        
        // Save state even on error
        if (g_marlin && !g_shutdown_requested) {
            try {
                saveStateToJSON();
            } catch (...) {
                std::cerr << "Failed to save state during error cleanup" << std::endl;
            }
        }
        
        cleanupAllButtons(); // Cleanup on error too
        return 1;
    }

    std::cout << "Shutdown complete." << std::endl;
    return 0;
}
