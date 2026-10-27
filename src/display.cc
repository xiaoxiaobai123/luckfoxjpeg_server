#include "display.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdint.h>

// spidev ioctls (linux/spi/spidev.h), spelled out so no kernel header is needed
#define SPI_IOC_WR_MODE _IOW('k', 1, uint8_t)
#define SPI_IOC_WR_BITS_PER_WORD _IOW('k', 3, uint8_t)
#define SPI_IOC_WR_MAX_SPEED_HZ _IOW('k', 4, uint32_t)

Display::~Display() {
    if (fd_ >= 0) close(fd_);
}

void Display::command(unsigned char c) {
    dc_.write(0);
    (void)::write(fd_, &c, 1);
}

void Display::data(const unsigned char* p, size_t n) {
    dc_.write(1);
    while (n > 0) {
        size_t k = n < chunk_ ? n : chunk_;
        ssize_t w = ::write(fd_, p, k);
        if (w <= 0) return;
        p += w;
        n -= (size_t)w;
    }
}

void Display::commandData(unsigned char c, const unsigned char* p, size_t n) {
    command(c);
    if (n) data(p, n);
}

bool Display::open(const char* spidev, int pinDc, int pinRst, int pinBl, int hz) {
    fd_ = ::open(spidev, O_WRONLY);
    if (fd_ < 0) return false;
    uint8_t mode = 0, bits = 8;
    uint32_t speed = (uint32_t)hz;
    ioctl(fd_, SPI_IOC_WR_MODE, &mode);
    ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed);

    // spidev refuses single writes larger than its bufsiz module parameter (4096 by default)
    FILE* f = fopen("/sys/module/spidev/parameters/bufsiz", "r");
    if (f) {
        unsigned long b = 0;
        if (fscanf(f, "%lu", &b) == 1 && b >= 256) chunk_ = (size_t)b;
        fclose(f);
    }

    // a negative backlight pin means the backlight is wired straight to 3.3V
    if (!dc_.openOut(pinDc, 0) || !rst_.openOut(pinRst, 1) || (pinBl >= 0 && !bl_.openOut(pinBl, 0))) {
        close(fd_);
        fd_ = -1;
        return false;
    }

    // hardware reset (the line is already high from openOut); the panel needs 120 ms before "sleep out"
    rst_.write(0);
    usleep(10000);
    rst_.write(1);
    usleep(120000);

    command(0x11);          // sleep out (a software reset on top of the hardware reset is not needed)
    usleep(100000);
    const unsigned char colmod = 0x55;  // 16 bits per pixel
    commandData(0x3A, &colmod, 1);
    const unsigned char madctl = 0x00;  // RGB order, no rotation
    commandData(0x36, &madctl, 1);
    command(0x21);          // display inversion on (needed by this panel)
    command(0x29);          // display on
    usleep(20000);

    buf_.resize((size_t)W * H * 2);
    // the backlight is switched on by the caller after the first picture is in the panel (no flash of old content)
    return true;
}

void Display::show(const cv::Mat& bgr) {
    if (fd_ < 0 || bgr.cols != W || bgr.rows != H || bgr.type() != CV_8UC3) return;
    unsigned char* out = &buf_[0];
    for (int y = 0; y < H; ++y) {
        const unsigned char* row = bgr.ptr<unsigned char>(y);
        for (int x = 0; x < W; ++x) {
            unsigned b = row[3 * x], g = row[3 * x + 1], r = row[3 * x + 2];
            unsigned v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
            *out++ = (unsigned char)(v >> 8);  // the panel takes the high byte first
            *out++ = (unsigned char)(v & 0xFF);
        }
    }
    const unsigned char col[4] = {0, 0, (unsigned char)((W - 1) >> 8), (unsigned char)((W - 1) & 0xFF)};
    const unsigned char rowa[4] = {0, 0, (unsigned char)((H - 1) >> 8), (unsigned char)((H - 1) & 0xFF)};
    commandData(0x2A, col, 4);
    commandData(0x2B, rowa, 4);
    command(0x2C);
    data(&buf_[0], buf_.size());
}
