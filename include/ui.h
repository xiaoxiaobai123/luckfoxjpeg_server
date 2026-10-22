#ifndef UI_H
#define UI_H

#include <opencv2/core/core.hpp>
#include <string>
#include <vector>
#include "keys.h"
#include "settings.h"

// What the capture thread hands to the screen.
struct Snapshot {
    cv::Mat preview;                 // 240x180 BGR camera picture
    std::vector<cv::Vec3f> circles;  // in preview pixels
    int count = 0;
    int result = -1;                 // -1 none yet, 0 NG, 1 OK, 2 error
    double fps = 0;
    double detectMs = 0;
};

// Run screen and settings menu for the 240x284 display. Keys: OK opens the menu, UP/DOWN pick an
// item, LEFT/RIGHT change the value, ADJ toggles x10 steps, OK on "Save" or "Discard" leaves it.
class Ui {
public:
    Ui(SharedSettings& shared, const std::string& confPath);

    void onKey(Key k);
    bool menuOpen() const { return menu_; }
    void render(cv::Mat& out, const Snapshot& snap);  // out becomes 240x284 BGR

private:
    void changeValue(int dir);
    std::string itemText(int i, const Settings& s) const;

    SharedSettings& shared_;
    std::string confPath_;
    bool menu_;
    int sel_;
    bool fast_;
    Settings backup_;
    std::string message_;
    long long messageUntil_;
};

#endif // UI_H
