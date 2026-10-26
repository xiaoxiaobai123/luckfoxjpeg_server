#ifndef TOUCH_H
#define TOUCH_H

// CST816D capacitive touch controller (Waveshare 1.83" module) on an I2C bus, polled by a thread.
// Events are queued; the same queue also takes taps injected from the browser bench page.
struct TouchEvent {
    int x = 0, y = 0;       // screen pixels, 240 x 284
    bool down = false;      // finger on the glass (false = lifted)
    bool repeat = false;    // finger kept down: sent again every ~100 ms after 450 ms
};

bool touchStart(const char* i2cDev, int resetPin);  // resetPin < 0: no reset line
void touchStop();
bool touchPop(TouchEvent& e);
void touchPush(int x, int y, bool down, bool repeat);  // for the bench page

#endif // TOUCH_H
