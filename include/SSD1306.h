#ifndef SSD1306_H
#define SSD1306_H

#include <stdint.h>
#include <string>

class SSD1306 {
private:
    int i2c_fd;
    uint8_t i2c_addr;
    uint8_t width;
    uint8_t height;
    uint8_t* buffer;
    
    // SSD1306 Commands
    static constexpr uint8_t SSD1306_SETCONTRAST = 0x81;
    static constexpr uint8_t SSD1306_DISPLAYALLON_RESUME = 0xA4;
    static constexpr uint8_t SSD1306_DISPLAYALLON = 0xA5;
    static constexpr uint8_t SSD1306_NORMALDISPLAY = 0xA6;
    static constexpr uint8_t SSD1306_INVERTDISPLAY = 0xA7;
    static constexpr uint8_t SSD1306_DISPLAYOFF = 0xAE;
    static constexpr uint8_t SSD1306_DISPLAYON = 0xAF;
    static constexpr uint8_t SSD1306_SETDISPLAYOFFSET = 0xD3;
    static constexpr uint8_t SSD1306_SETCOMPINS = 0xDA;
    static constexpr uint8_t SSD1306_SETVCOMDETECT = 0xDB;
    static constexpr uint8_t SSD1306_SETDISPLAYCLOCKDIV = 0xD5;
    static constexpr uint8_t SSD1306_SETPRECHARGE = 0xD9;
    static constexpr uint8_t SSD1306_SETMULTIPLEX = 0xA8;
    static constexpr uint8_t SSD1306_SETLOWCOLUMN = 0x00;
    static constexpr uint8_t SSD1306_SETHIGHCOLUMN = 0x10;
    static constexpr uint8_t SSD1306_SETSTARTLINE = 0x40;
    static constexpr uint8_t SSD1306_MEMORYMODE = 0x20;
    static constexpr uint8_t SSD1306_COLUMNADDR = 0x21;
    static constexpr uint8_t SSD1306_PAGEADDR = 0x22;
    static constexpr uint8_t SSD1306_COMSCANINC = 0xC0;
    static constexpr uint8_t SSD1306_COMSCANDEC = 0xC8;
    static constexpr uint8_t SSD1306_SEGREMAP = 0xA0;
    static constexpr uint8_t SSD1306_CHARGEPUMP = 0x8D;
    
    bool writeCommand(uint8_t cmd);
    bool writeData(uint8_t* data, size_t length);
    
public:
    SSD1306(uint8_t address = 0x3C, uint8_t w = 128, uint8_t h = 64);
    ~SSD1306();
    
    bool init();
    void clear();
    void display();
    void setPixel(uint8_t x, uint8_t y, bool on = true);
    void drawChar(uint8_t x, uint8_t y, char c, bool invert = false);
    void drawString(uint8_t x, uint8_t y, const std::string& str, bool invert = false);
    void drawLine(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, bool on = true);
    void drawRect(uint8_t x, uint8_t y, uint8_t w, uint8_t h, bool fill = false, bool on = true);
    void setCursor(uint8_t x, uint8_t y);
    void print(const std::string& text, bool invert = false);
    
    // Menu specific functions
    void clearLine(uint8_t line);
    void printLine(uint8_t line, const std::string& text, bool invert = false);
    void printMenuLine(uint8_t line, const std::string& text, bool selected = false);
    
    uint8_t getWidth() const { return width; }
    uint8_t getHeight() const { return height; }
    uint8_t getTextRows() const { return height / 8; }
    uint8_t getTextCols() const { return width / 8; }
};

#endif // SSD1306_H
