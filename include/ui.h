#ifndef UI_H
#define UI_H

#include <opencv2/core/core.hpp>
#include <string>
#include <vector>
#include "keys.h"
#include "settings.h"
#include "touch.h"

// What the capture thread hands to the screen.
struct Snapshot {
    cv::Mat preview;                 // 379x284 BGR camera picture (full screen height)
    std::vector<cv::Vec3f> circles;  // in preview pixels (centre x, centre y, radius)
    int count = 0;
    int result = -1;                 // -1 none yet, 0 NG, 1 OK, 2 error
    double fps = 0;
    double detectMs = 0;
    std::string status;              // shown instead of the picture when there is none (start-up, camera missing)
};

// Run screen and settings menu for the 240x284 display.
//   keys:  OK opens the menu, UP/DOWN pick an item, LEFT/RIGHT change the value, ADJ toggles x10 steps,
//          OK on "Save" asks to be pressed again, OK on "Discard" leaves at once.
//   touch: tap opens the menu, tap a row to select it, tap the < or > ends of a value row to change it
//          (hold to repeat), tap the header for x10 steps, tap Save twice, tap Discard.
class Ui {
public:
    Ui(SharedSettings& shared, const std::string& confPath);

    void onKey(Key k);
    void onTouch(const TouchEvent& e);
    bool menuOpen() const { return menu_; }
    void setTouchDebug(bool on) { touchDebug_ = on; }  // draw a marker where the touch controller says the finger is
    void render(cv::Mat& out, const Snapshot& snap);  // out becomes 240x284 BGR

private:
    void renderScreen(cv::Mat& out, const Snapshot& snap);
    void enterMenu();
    void changeValue(int dir);
    void activate();       // OK on the selected row
    int firstRow() const;  // index of the first menu row on screen
    std::string itemText(int i, const Settings& s) const;

    SharedSettings& shared_;
    std::string confPath_;
    bool menu_;
    int sel_;
    bool fast_;
    Settings backup_;
    std::string message_;
    long long messageUntil_;
    long long saveConfirmUntil_;
    bool touchDebug_ = false;
    int lastTouchX_ = 0, lastTouchY_ = 0;
    long long lastTouchAt_ = 0;
};

#endif // UI_H
