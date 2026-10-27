#include "keys.h"
#include "gpio.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <deque>

static pthread_mutex_t g_qLock = PTHREAD_MUTEX_INITIALIZER;
static std::deque<Key> g_queue;
static volatile bool g_scan = false;
static pthread_t g_thread;
static bool g_threadStarted = false;

void keysPush(Key k) {
    pthread_mutex_lock(&g_qLock);
    if (g_queue.size() < 16) g_queue.push_back(k);
    pthread_mutex_unlock(&g_qLock);
}

bool keysPop(Key& k) {
    pthread_mutex_lock(&g_qLock);
    bool have = !g_queue.empty();
    if (have) {
        k = g_queue.front();
        g_queue.pop_front();
    }
    pthread_mutex_unlock(&g_qLock);
    return have;
}

bool keysPushByName(const std::string& n) {
    Key k = K_NONE;
    if (n == "ok") k = K_OK;
    else if (n == "up") k = K_UP;
    else if (n == "down") k = K_DOWN;
    else if (n == "left") k = K_LEFT;
    else if (n == "right") k = K_RIGHT;
    else if (n == "adj") k = K_ADJ;
    if (k == K_NONE) return false;
    keysPush(k);
    return true;
}

// ---- scanning thread ----

static Gpio g_gpioKeys[6];
static bool g_gpioFit[6];
static int g_gpioPin[6] = {-1, -1, -1, -1, -1, -1};  // wanted pins, retried by the scan thread if opening failed
static int g_adcFd = -1;
static bool g_useAdc = false;
static bool g_activeLow = false;  // game pad: common wire on ground, 10k pull-ups, a pressed key reads 0
static bool g_mixed = false;  // direct keys on GPIOs plus two keys (OK = 1k, ADJ = 3.3k) on the ADC

static const Key kOrder[6] = {K_OK, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_ADJ};

// Resistor ladder: 10k pull-up to 1.8V, key resistors 1k/3.3k/6.8k/15k/33k/68k to ground.
// Expected 10-bit counts for one key: 93, 254, 413, 613, 785, 892; idle 1023.
// Two or more keys at once put their resistors in parallel, which only lowers the reading, so the key with the
// smallest resistor wins (OK > UP > DOWN > LEFT > RIGHT > ADJ). The limits are set so that every combination
// (two, three, four or five keys) lands in the band of that winning key, e.g. DOWN+LEFT reads 326 and must stay
// DOWN, UP+DOWN+LEFT reads 166 and must stay UP. Use 1 % resistors: the margins are about 15 counts.
static Key classifyAdc(int raw) {
    if (raw < 140) return K_OK;
    if (raw < 275) return K_UP;
    if (raw < 470) return K_DOWN;
    if (raw < 660) return K_LEFT;
    if (raw < 838) return K_RIGHT;
    if (raw < 957) return K_ADJ;
    return K_NONE;
}

static Key readRaw() {
    // keys wired straight to a GPIO (key between the pin and 3.3 V) come first
    for (int i = 0; i < 6; ++i)
        if (g_gpioFit[i] && g_gpioKeys[i].read() == (g_activeLow ? 0 : 1)) return kOrder[i];
    if (g_useAdc) {
        char buf[16] = {0};
        if (g_adcFd < 0) return K_NONE;
        ssize_t n = pread(g_adcFd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) return K_NONE;
        int v = atoi(buf);
        if (g_activeLow) return v < 400 ? K_OK : K_NONE;  // pad centre key: 10k pull-up to 1.8 V, key to ground
        Key k = classifyAdc(v);
        if (g_mixed) {  // only two keys on the ladder: 1k reads ~93 (OK), 3.3k reads ~254 (ADJ)
            if (k == K_OK) return K_OK;
            if (k == K_UP) return K_ADJ;
            return K_NONE;
        }
        return k;
    }
    return K_NONE;
}

static long long nowMs() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void* scanThread(void*) {
    Key stable = K_NONE, candidate = K_NONE;
    int same = 0;
    long long pressedAt = 0, lastRepeat = 0, lastRetry = 0;
    while (g_scan) {
        long long now = nowMs();
        if (now - lastRetry > 1000) {  // a key pin that was busy at start-up (boot) is picked up later
            lastRetry = now;
            for (int i = 0; i < 6; ++i)
                if (g_gpioPin[i] >= 0 && !g_gpioFit[i] && g_gpioKeys[i].openIn(g_gpioPin[i], "none")) g_gpioFit[i] = true;
        }
        Key raw = readRaw();
        if (raw == candidate) {
            if (same < 100) ++same;
        } else {
            candidate = raw;
            same = 1;
        }
        if (same >= 4 && candidate != stable) {  // ~20 ms of the same reading = debounced
            stable = candidate;
            if (stable != K_NONE) {
                keysPush(stable);
                pressedAt = lastRepeat = nowMs();
            }
        }
        // hold an arrow key to repeat
        if (stable == K_UP || stable == K_DOWN || stable == K_LEFT || stable == K_RIGHT) {
            long long t = nowMs();
            long long held = t - pressedAt;
            if (held > 450 && t - lastRepeat > (held > 1500 ? 35 : 90)) {  // faster the longer it is held
                keysPush(stable);
                lastRepeat = t;
            }
        }
        usleep(5000);
    }
    return NULL;
}

static bool startThread() {
    g_scan = true;
    if (pthread_create(&g_thread, NULL, scanThread, NULL) != 0) {
        g_scan = false;
        return false;
    }
    g_threadStarted = true;
    return true;
}

bool keysStartGpio(const int pins[6]) {
    g_useAdc = false;
    bool any = false;
    for (int i = 0; i < 6; ++i) {
        g_gpioFit[i] = false;
        g_gpioPin[i] = pins[i];
        if (pins[i] < 0) continue;
        if (g_gpioKeys[i].openIn(pins[i], "none")) {
            g_gpioFit[i] = true;
            any = true;
        }
    }
    return any && startThread();
}

// Mixed wiring: pins[] gives a GPIO for each of OK, UP, DOWN, LEFT, RIGHT, ADJ, or -1. The keys without a pin
// (normally OK and ADJ) share the ADC ladder: OK through 1k, ADJ through 3.3k.
bool keysStartMixed(const int pins[6], int channel, bool activeLow) {
    g_mixed = true;
    g_activeLow = activeLow;
    bool any = false;
    for (int i = 0; i < 6; ++i) {
        g_gpioFit[i] = false;
        g_gpioPin[i] = pins[i];
        if (pins[i] < 0) continue;
        if (g_gpioKeys[i].openIn(pins[i], "none")) {
            g_gpioFit[i] = true;
            any = true;
        }
    }
    if (channel >= 0) {  // channel -1: all keys are on GPIOs
        g_useAdc = true;
        char path[96];
        snprintf(path, sizeof(path), "/sys/bus/iio/devices/iio:device0/in_voltage%d_raw", channel);
        g_adcFd = open(path, O_RDONLY);
        if (g_adcFd >= 0) any = true;
    }
    return any && startThread();
}

bool keysStartAdc(int channel) {
    g_useAdc = true;
    char path[96];
    snprintf(path, sizeof(path), "/sys/bus/iio/devices/iio:device0/in_voltage%d_raw", channel);
    g_adcFd = open(path, O_RDONLY);
    if (g_adcFd < 0) return false;
    return startThread();
}

void keysStop() {
    if (g_threadStarted) {
        g_scan = false;
        pthread_join(g_thread, NULL);
        g_threadStarted = false;
    }
    if (g_adcFd >= 0) close(g_adcFd);
    g_adcFd = -1;
}
