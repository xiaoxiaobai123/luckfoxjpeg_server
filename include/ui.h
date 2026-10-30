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
    cv::Mat preview;                 // 240x180 BGR camera picture: the whole frame scaled to the screen width
    std::vector<cv::Vec3f> circles;  // in preview pixels (centre x, centre y, radius)
    int count = 0;
    int result = -1;                 // -1 none yet, 0 NG, 1 OK, 2 error
    double fps = 0;
    double detectMs = 0;
    double cpuPct = 0, ramUsedMb = 0, ramTotalMb = 0, tempC = -1;  // system load, shown in the menu header
    double totalMs = 0;              // from the PLC trigger edge to the result
    int trigNo = 0;                  // number of the trigger (counts up, shows that a trigger really arrived)
    cv::Mat roiView;                 // the detection area enlarged to fit the screen (empty when it is the whole picture)
    std::vector<cv::Vec3f> roiCircles;  // circles in roiView pixels
    bool busy = false;               // a trigger has arrived, the result is not there yet
    bool held = false;               // frozen picture of the last PLC-triggered measurement (not live)
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
    void setTouchAvailable(bool on) { touchAvailable_ = on; }  // false: show key hints instead of tap hints
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
    mutable int top_ = 0;  // first menu row on screen
    bool fast_;
    Settings backup_;
    std::string message_;
    long long messageUntil_;
    long long saveConfirmUntil_;
    int lastTrigNo_ = 0;           // trigger number seen on the last frame: a new one starts the light sweep
    long long trigShownAt_ = 0;
    long long resultAt_ = 0;       // when the result of that trigger appeared
    bool wasBusy_ = false;
    bool touchDebug_ = false;
    bool touchAvailable_ = true;
    int lastTouchX_ = 0, lastTouchY_ = 0;
    long long lastTouchAt_ = 0;
};

#endif // UI_H
