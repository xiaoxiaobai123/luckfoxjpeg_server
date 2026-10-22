#ifndef GPIO_H
#define GPIO_H

// Minimal sysfs GPIO helper. Pin number = bank * 32 + group * 8 + index
// (A=0, B=1, C=2, D=3), e.g. GPIO1_C7 = 1 * 32 + 2 * 8 + 7 = 55.

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <string>

class Gpio {
public:
    Gpio() : fd_(-1), pin_(-1) {}
    ~Gpio() { if (fd_ >= 0) close(fd_); }

    bool openOut(int pin, int initial = 0) {
        if (!setup(pin, "out")) return false;
        write(initial);
        return true;
    }

    // edge: "rising", "falling", "both" or "none"
    bool openIn(int pin, const char* edge = "rising") {
        if (!setup(pin, "in")) return false;
        return writeAttr("edge", edge);
    }

    void write(int value) {
        if (fd_ < 0) return;
        const char c = value ? '1' : '0';
        (void)::pwrite(fd_, &c, 1, 0);
    }

    int read() {
        if (fd_ < 0) return -1;
        char c = '0';
        if (::pread(fd_, &c, 1, 0) != 1) return -1;
        return c == '1' ? 1 : 0;
    }

    // Wait for an edge. Returns true if an edge arrived within timeoutMs.
    bool waitEdge(int timeoutMs) {
        if (fd_ < 0) return false;
        (void)read();  // clear any pending event
        struct pollfd pfd;
        pfd.fd = fd_;
        pfd.events = POLLPRI | POLLERR;
        pfd.revents = 0;
        return poll(&pfd, 1, timeoutMs) > 0;
    }

private:
    bool setup(int pin, const char* dir) {
        pin_ = pin;
        char path[64];
        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", pin);
        if (access(path, F_OK) != 0) {
            FILE* f = fopen("/sys/class/gpio/export", "w");
            if (!f) return false;
            fprintf(f, "%d", pin);
            fclose(f);
            for (int i = 0; i < 50 && access(path, F_OK) != 0; ++i) usleep(10000);
        }
        if (!writeAttr("direction", dir)) return false;
        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
        fd_ = open(path, O_RDWR);
        return fd_ >= 0;
    }

    bool writeAttr(const char* name, const char* value) {
        char path[96];
        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/%s", pin_, name);
        FILE* f = fopen(path, "w");
        if (!f) return false;
        fputs(value, f);
        fclose(f);
        return true;
    }

    int fd_;
    int pin_;
};

#endif // GPIO_H
