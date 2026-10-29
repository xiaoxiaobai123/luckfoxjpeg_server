#ifndef MATCH_H
#define MATCH_H

#include <opencv2/core/core.hpp>
#include <string>
#include "settings.h"

// Template matching inside the detection area: one grey template, searched with a small rotation range.
struct Template {
    cv::Mat img;  // grey, w x h
    bool valid() const { return !img.empty(); }
};

struct MatchResult {
    bool found = false;   // false: no template, or the template does not fit into the detection area
    double score = 0;     // 0..100, zero-mean normalised correlation (insensitive to brightness and contrast)
    double angle = 0;     // degrees, best rotation found
    cv::Point2f center;   // camera pixels
};

bool tplLearn(const cv::Mat& gray, const Settings& s, Template& t);  // cut the template area out of the frame
bool tplSave(const Template& t, const std::string& path);
bool tplLoad(Template& t, const std::string& path);
MatchResult tplMatch(const cv::Mat& gray, const Settings& s, const Template& t);

#endif // MATCH_H
