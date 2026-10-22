#ifndef SETTINGS_H
#define SETTINGS_H

#include <pthread.h>

// User-adjustable detection settings, edited from the on-screen menu and stored in a small text file.
struct Settings {
    int target = 5;       // circle count that means OK (front)
    int minDia = 32;      // smallest circle diameter, pixels in the 640x480 camera image
    int maxDia = 240;     // largest circle diameter, same unit
    int roundness = 25;   // Hough accumulator threshold: higher = only rounder circles
    int edge = 100;       // Canny high threshold: lower = more sensitive to weak edges
    int exposure = 0;     // exposure time in 0.1 ms units, 0 = auto

    void clamp();
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

private:
    pthread_mutex_t m_;
    Settings s_;
    unsigned ver_;
};

#endif // SETTINGS_H
