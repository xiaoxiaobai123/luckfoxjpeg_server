#include "touch.h"
#include "gpio.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <deque>

// i2c-dev ioctls (linux/i2c-dev.h, linux/i2c.h), spelled out so no kernel header is needed
#define I2C_SLAVE_FORCE_ 0x0706
#define I2C_RDWR_ 0x0707
struct I2cMsg {
    uint16_t addr;
    uint16_t flags;
    uint16_t len;
    uint8_t* buf;
};
struct I2cRdwr {
    I2cMsg* msgs;
    uint32_t nmsgs;
};

static const uint8_t kAddr = 0x15;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::deque<TouchEvent> g_queue;
static volatile bool g_run = false;
static pthread_t g_thread;
static bool g_started = false;
static int g_fd = -1;
static Gpio g_rst;

static long long nowMs() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

void touchPush(int x, int y, bool down, bool repeat) {
    TouchEvent e;
    e.x = x;
    e.y = y;
    e.down = down;
    e.repeat = repeat;
    pthread_mutex_lock(&g_lock);
    if (g_queue.size() < 32) g_queue.push_back(e);
    pthread_mutex_unlock(&g_lock);
}

bool touchPop(TouchEvent& e) {
    pthread_mutex_lock(&g_lock);
    bool have = !g_queue.empty();
    if (have) {
        e = g_queue.front();
        g_queue.pop_front();
    }
    pthread_mutex_unlock(&g_lock);
    return have;
}

// register read with a repeated start
static bool readReg(uint8_t reg, uint8_t* out, int n) {
    I2cMsg msgs[2];
    msgs[0].addr = kAddr;
    msgs[0].flags = 0;
    msgs[0].len = 1;
    msgs[0].buf = &reg;
    msgs[1].addr = kAddr;
    msgs[1].flags = 1;  // read
    msgs[1].len = (uint16_t)n;
    msgs[1].buf = out;
    I2cRdwr d;
    d.msgs = msgs;
    d.nmsgs = 2;
    return ioctl(g_fd, I2C_RDWR_, &d) >= 0;
}

static bool writeReg(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    I2cMsg m;
    m.addr = kAddr;
    m.flags = 0;
    m.len = 2;
    m.buf = buf;
    I2cRdwr d;
    d.msgs = &m;
    d.nmsgs = 1;
    return ioctl(g_fd, I2C_RDWR_, &d) >= 0;
}

static void* pollThread(void*) {
    bool wasDown = false;
    int lastX = 0, lastY = 0;
    long long downAt = 0, lastRepeat = 0;
    while (g_run) {
        uint8_t r[6];  // registers 0x01..0x06: gesture, finger count, X high, X low, Y high, Y low
        if (readReg(0x01, r, 6)) {
            int fingers = r[1];
            int x = ((r[2] & 0x0F) << 8) | r[3];
            int y = ((r[4] & 0x0F) << 8) | r[5];
            if (fingers > 0) {
                long long t = nowMs();
                if (!wasDown) {
                    touchPush(x, y, true, false);
                    wasDown = true;
                    downAt = lastRepeat = t;
                } else if (t - downAt > 450 && t - lastRepeat > 100) {
                    touchPush(x, y, true, true);
                    lastRepeat = t;
                }
                lastX = x;
                lastY = y;
            } else if (wasDown) {
                touchPush(lastX, lastY, false, false);
                wasDown = false;
            }
        }
        usleep(20000);
    }
    return NULL;
}

bool touchStart(const char* i2cDev, int resetPin) {
    g_fd = open(i2cDev, O_RDWR);
    if (g_fd < 0) return false;
    ioctl(g_fd, I2C_SLAVE_FORCE_, kAddr);

    if (resetPin >= 0 && g_rst.openOut(resetPin, 1)) {
        g_rst.write(0);
        usleep(10000);
        g_rst.write(1);
        usleep(80000);  // the chip needs a moment after reset before it answers
    }
    uint8_t probe[1];
    bool answers = false;
    for (int i = 0; i < 10 && !answers; ++i) {
        answers = readReg(0x01, probe, 1);
        if (!answers) usleep(30000);
    }
    if (!answers) {
        close(g_fd);
        g_fd = -1;
        return false;
    }
    writeReg(0xFE, 0x01);  // do not fall asleep after a few seconds without touches

    g_run = true;
    if (pthread_create(&g_thread, NULL, pollThread, NULL) != 0) {
        g_run = false;
        close(g_fd);
        g_fd = -1;
        return false;
    }
    g_started = true;
    return true;
}

void touchStop() {
    if (g_started) {
        g_run = false;
        pthread_join(g_thread, NULL);
        g_started = false;
    }
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
}
