#include "match.h"

#include <math.h>
#include <stdio.h>
#include <algorithm>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

namespace {

// 255 inside the template shape (whole rectangle, or the ellipse inscribed in it), 0 outside
cv::Mat shapeMask(int w, int h, int shape) {
    cv::Mat m(h, w, CV_8UC1, cv::Scalar(shape == 1 ? 0 : 255));
    if (shape == 1) cv::ellipse(m, cv::Point(w / 2, h / 2), cv::Size(std::max(1, w / 2), std::max(1, h / 2)), 0, 0, 360, cv::Scalar(255), -1);
    return m;
}

cv::Mat rotated(const cv::Mat& src, double angle, bool isMask) {
    if (angle == 0) return src;
    cv::Mat M = cv::getRotationMatrix2D(cv::Point2f(src.cols / 2.0f, src.rows / 2.0f), angle, 1.0);
    cv::Mat dst;
    if (isMask) {
        cv::warpAffine(src, dst, M, src.size(), cv::INTER_NEAREST, cv::BORDER_CONSTANT, cv::Scalar(0));
        cv::threshold(dst, dst, 127, 255, cv::THRESH_BINARY);
    } else {
        cv::warpAffine(src, dst, M, src.size(), cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    }
    return dst;
}

// best correlation (-1..1) of the masked template anywhere in 'search'; loc = top-left of the best place
double matchOnce(const cv::Mat& search, const cv::Mat& tpl, const cv::Mat& mask, bool useMask, cv::Point& loc) {
    cv::Mat res;
    if (useMask) {
        try {
            cv::matchTemplate(search, tpl, res, cv::TM_CCOEFF_NORMED, mask);
            res.setTo(-1.0, res > 1.001);  // the masked version returns garbage (>1) on flat, saturated areas
        } catch (const cv::Exception&) {  // an OpenCV build without masked matching: use the whole rectangle
            cv::matchTemplate(search, tpl, res, cv::TM_CCOEFF_NORMED);
        }
    } else {
        cv::matchTemplate(search, tpl, res, cv::TM_CCOEFF_NORMED);
    }
    cv::patchNaNs(res, -1.0);  // a flat patch has no correlation
    double mx = -1;
    cv::minMaxLoc(res, NULL, &mx, NULL, &loc);
    if (!(mx == mx)) mx = -1;
    return mx;
}

}  // namespace

bool tplLearn(const cv::Mat& gray, const Settings& s, Template& t) {
    cv::Rect r(s.tplX, s.tplY, s.tplW, s.tplH);
    r &= cv::Rect(0, 0, gray.cols, gray.rows);
    if (r.width < 8 || r.height < 8) return false;
    gray(r).copyTo(t.img);
    return true;
}

bool tplSave(const Template& t, const std::string& path) {
    if (!t.valid()) return false;
    std::string tmp = path + ".tmp.png";
    if (!cv::imwrite(tmp, t.img)) return false;
    return rename(tmp.c_str(), path.c_str()) == 0;
}

bool tplLoad(Template& t, const std::string& path) {
    cv::Mat m = cv::imread(path, cv::IMREAD_GRAYSCALE);
    if (m.empty() || m.cols < 8 || m.rows < 8) return false;
    t.img = m;
    return true;
}

MatchResult tplMatch(const cv::Mat& gray, const Settings& s, const Template& t) {
    MatchResult r;
    if (!t.valid()) return r;
    cv::Rect roi(s.roiX, s.roiY, s.roiW, s.roiH);
    roi &= cv::Rect(0, 0, gray.cols, gray.rows);
    if (roi.width < 8 || roi.height < 8) roi = cv::Rect(0, 0, gray.cols, gray.rows);
    if (t.img.cols > roi.width || t.img.rows > roi.height) return r;  // does not fit
    cv::Mat search = gray(roi);
    cv::Mat mask = shapeMask(t.img.cols, t.img.rows, s.tplShape);
    const bool circ = s.tplShape == 1;  // a rectangle needs no mask (faster, and no garbage values)
    const int A = s.tplAngle;  // search -A..+A degrees

    // 1. coarse: half size, every whole degree
    const int f = std::min(t.img.cols, t.img.rows) >= 24 ? 2 : 1;
    cv::Mat sc = search, tc = t.img, mc = mask;
    if (f > 1) {
        cv::resize(search, sc, cv::Size(search.cols / f, search.rows / f), 0, 0, cv::INTER_AREA);
        cv::resize(t.img, tc, cv::Size(t.img.cols / f, t.img.rows / f), 0, 0, cv::INTER_AREA);
        cv::resize(mask, mc, tc.size(), 0, 0, cv::INTER_NEAREST);
    }
    double best = -2, bestAngle = 0;
    cv::Point bestLoc;
    const int stepA = A >= 3 ? 2 : 1;  // coarse step; the fine stage below corrects up to +-1 degree
    for (int a = -A; a <= A; a += stepA) {
        cv::Point loc;
        double v = matchOnce(sc, rotated(tc, a, false), cv::Mat(), false, loc);  // searching is always unmasked (fast)
        if (v > best) { best = v; bestAngle = a; bestLoc = loc; }
    }

    // 2. fine: full size, a small window around the coarse position, half-degree steps
    {
        int wx = std::min(std::max(bestLoc.x * f - 4, 0), search.cols - t.img.cols);
        int wy = std::min(std::max(bestLoc.y * f - 4, 0), search.rows - t.img.rows);
        cv::Rect win(wx, wy, std::min(search.cols - wx, t.img.cols + 9), std::min(search.rows - wy, t.img.rows + 9));
        cv::Mat ws = search(win);
        double fineBest = -2, fineAngle = bestAngle;
        cv::Point fineLoc = bestLoc;
        const double steps[5] = {0.0, -0.5, 0.5, -1.0, 1.0};
        for (int i = 0; i < (A == 0 ? 1 : (stepA == 2 ? 5 : 3)); ++i) {
            double a = bestAngle + steps[i];
            cv::Point loc;
            double v = matchOnce(ws, rotated(t.img, a, false), rotated(mask, a, true), circ, loc);
            if (v > fineBest) { fineBest = v; fineAngle = a; fineLoc = cv::Point(loc.x + wx, loc.y + wy); }
        }
        best = fineBest; bestAngle = fineAngle; bestLoc = fineLoc;
    }

    r.found = true;
    r.score = std::min(100.0, std::max(0.0, best * 100.0));
    r.angle = bestAngle;
    r.center = cv::Point2f(roi.x + bestLoc.x + t.img.cols / 2.0f, roi.y + bestLoc.y + t.img.rows / 2.0f);
    return r;
}
