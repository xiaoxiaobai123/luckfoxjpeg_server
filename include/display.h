#ifndef DISPLAY_H
#define DISPLAY_H

#include <opencv2/core/core.hpp>
#include <vector>
#include "gpio.h"

// 1.83" 240x284 SPI LCD. Waveshare's own ESP-IDF demo drives it with the stock ST7789
// command set, 40 MHz, SPI mode 0, RGB565, inverted colours and no window offset, which is
// what this driver reproduces.
class Display {
public:
    Display() : fd_(-1), chunk_(4096) {}
    ~Display();

    bool open(const char* spidev, int pinDc, int pinRst, int pinBl, int hz);
    bool ok() const { return fd_ >= 0; }
    void show(const cv::Mat& bgr);  // must be 240 x 284, 3 channels
    void backlight(bool on) { bl_.write(on ? 1 : 0); }

    static const int W = 240;
    static const int H = 284;

private:
    void command(unsigned char c);
    void data(const unsigned char* p, size_t n);
    void commandData(unsigned char c, const unsigned char* p, size_t n);

    int fd_;
    size_t chunk_;
    Gpio dc_, rst_, bl_;
    std::vector<unsigned char> buf_;
};

#endif // DISPLAY_H
