#ifndef SETTINGS_H
#define SETTINGS_H

#include <pthread.h>
#include <string>

// User-adjustable detection settings, edited from the on-screen menu and stored in a small text file.
struct Settings {
    int mode = 1;         // 0 = circle (Hough, needs clear edges), 1 = dark hole (blob darker than its surroundings), 2 = template
    int target = 5;       // circle count that means OK (front)
    int minDia = 32;      // smallest circle diameter, pixels in the 640x480 camera image
    int maxDia = 240;     // largest circle diameter, same unit
    int roundness = 25;   // Hough accumulator threshold: higher = only rounder circles
    int edge = 100;       // Canny high threshold: lower = more sensitive to weak edges
    int contrast = 10;    // dark-hole mode: how much darker than the surroundings a pixel must be (grey levels)
    int shape = 65;       // dark-hole mode: how round the dark spot must be, percent (100 = perfect circle)
    int lang = 0;         // menu language: 0 = English, 1 = Chinese
    int exposure = 0;     // exposure time in 0.1 ms units, 0 = auto
    int gain = 0;         // sensor gain in 0.1x units, 0 = auto (fix both exposure and gain for a steady picture)
    int roiX = 0, roiY = 0, roiW = 640, roiH = 480;  // detection area in camera pixels, the rest is ignored

    // template mode: one learned picture is searched in the detection area, OK when it is found with a high score
    int tplScore = 85;    // lowest match score (0..100) that counts as OK
    int tplCount = 1;     // places that must match: 1 = one, 0 = any number (at least one), up to 30
    int tplAngle = 3;     // the template may be turned by up to this many degrees either way
    int tplShape = 0;     // 0 = rectangle, 1 = circle (ellipse inscribed in the rectangle)
    int tplX = 280, tplY = 200, tplW = 64, tplH = 64;  // where the template is cut out of the picture when learned

    void clamp();
    bool setKey(const char* key, int v);  // by the name used in the settings file; false for an unknown name
    std::string toJson() const;           // {"mode":1,...} with the names of the settings file
    bool load(const char* path);        // false if the file is missing or unreadable
    bool save(const char* path) const;  // writes a temp file, then renames it
};

// Settings shared between the UI thread (edits) and the capture thread (reads).
class SharedSettings {
public:
    SharedSettings();
    Settings get();
    void set(const Settings& s);
    unsigned version();

    // template learning: the menu asks, the capture thread does it on the next frame
    void requestLearn();
    bool takeLearn();
    void setLearnResult(int r);   // 1 = learned, -1 = failed
    int takeLearnResult();        // 0 when there is nothing new
    void setHaveTemplate(bool on);
    bool haveTemplate();

private:
    pthread_mutex_t m_;
    Settings s_;
    unsigned ver_;
    bool learnReq_;
    int learnRes_;
    bool haveTpl_;
};

#endif // SETTINGS_H
