#include "ui.h"
#include "logo.h"

#include <stdio.h>
#include <time.h>
#include <algorithm>
#include <opencv2/imgproc/imgproc.hpp>

namespace {

enum Item {
    IT_MODE, IT_TARGET, IT_MINDIA, IT_MAXDIA,
    IT_ROUND, IT_EDGE,        // circle mode only
    IT_CONTRAST, IT_SHAPE,    // dark-hole mode only
    IT_EXPOSURE, IT_GAIN, IT_AX, IT_AY, IT_AW, IT_AH,
    IT_SAVE, IT_DISCARD
};

const int kRows = 3;      // menu lines visible at once
const int kRowTop = 284 - kRows * 28;  // the list sits on the bottom edge of the screen
const int kRowH = 28;
const int kHeaderH = 30;  // header bar height: touching it toggles the x10 step

long long nowMs() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

bool isValueItem(int id) { return id != IT_SAVE && id != IT_DISCARD; }

// The rows that are shown depend on the detection mode.
std::vector<int> visibleItems(int mode) {
    std::vector<int> v;
    v.push_back(IT_MODE);
    v.push_back(IT_TARGET);
    v.push_back(IT_MINDIA);
    v.push_back(IT_MAXDIA);
    if (mode == 1) {
        v.push_back(IT_CONTRAST);
        v.push_back(IT_SHAPE);
    } else {
        v.push_back(IT_ROUND);
        v.push_back(IT_EDGE);
    }
    v.push_back(IT_EXPOSURE);
    v.push_back(IT_GAIN);
    v.push_back(IT_AX);
    v.push_back(IT_AY);
    v.push_back(IT_AW);
    v.push_back(IT_AH);
    v.push_back(IT_SAVE);
    v.push_back(IT_DISCARD);
    return v;
}

}  // namespace

Ui::Ui(SharedSettings& shared, const std::string& confPath)
    : shared_(shared), confPath_(confPath), menu_(false), sel_(0), fast_(false), messageUntil_(0),
      saveConfirmUntil_(0) {}

std::string Ui::itemText(int id, const Settings& s) const {
    char b[48];
    switch (id) {
    case IT_MODE: snprintf(b, sizeof(b), "Mode      %s", s.mode == 1 ? "Dark" : "Circle"); break;
    case IT_TARGET: snprintf(b, sizeof(b), "Count     %d", s.target); break;
    case IT_MINDIA: snprintf(b, sizeof(b), "Min dia   %d px", s.minDia); break;
    case IT_MAXDIA: snprintf(b, sizeof(b), "Max dia   %d px", s.maxDia); break;
    case IT_ROUND: snprintf(b, sizeof(b), "Roundness %d", s.roundness); break;
    case IT_EDGE: snprintf(b, sizeof(b), "Edge      %d", s.edge); break;
    case IT_CONTRAST: snprintf(b, sizeof(b), "Contrast  %d", s.contrast); break;
    case IT_SHAPE: snprintf(b, sizeof(b), "Shape     %d %%", s.shape); break;
    case IT_EXPOSURE:
        if (s.exposure == 0) snprintf(b, sizeof(b), "Exposure  auto");
        else snprintf(b, sizeof(b), "Exposure  %.1f ms", s.exposure / 10.0);
        break;
    case IT_GAIN:
        if (s.gain == 0) snprintf(b, sizeof(b), "Gain      auto");
        else snprintf(b, sizeof(b), "Gain      %.1fx", s.gain / 10.0);
        break;
    case IT_AX: snprintf(b, sizeof(b), "Area x    %d", s.roiX); break;
    case IT_AY: snprintf(b, sizeof(b), "Area y    %d", s.roiY); break;
    case IT_AW: snprintf(b, sizeof(b), "Area wide %d", s.roiW); break;
    case IT_AH: snprintf(b, sizeof(b), "Area high %d", s.roiH); break;
    case IT_SAVE:
        if (saveConfirmUntil_ > nowMs()) snprintf(b, sizeof(b), "[ Again to SAVE ]");
        else snprintf(b, sizeof(b), "[ Save and exit ]");
        break;
    default: snprintf(b, sizeof(b), "[ Discard changes ]"); break;
    }
    return b;
}

int Ui::firstRow() const {
    // wheel: the selected row stays in the middle of the window, the list turns around it (and wraps round)
    return sel_ - kRows / 2;
}

void Ui::enterMenu() {
    menu_ = true;
    sel_ = 0;
    top_ = 0;
    fast_ = false;
    saveConfirmUntil_ = 0;
    backup_ = shared_.get();
}

void Ui::changeValue(int dir) {
    Settings s = shared_.get();
    std::vector<int> items = visibleItems(s.mode);
    if (sel_ < 0 || sel_ >= (int)items.size()) return;
    int step = fast_ ? 10 : 1;
    switch (items[sel_]) {
    case IT_MODE: s.mode = dir > 0 ? 1 : 0; break;
    case IT_TARGET: s.target += dir * step; break;
    case IT_MINDIA: s.minDia += dir * step * 2; break;
    case IT_MAXDIA: s.maxDia += dir * step * 2; break;
    case IT_ROUND: s.roundness += dir * step; break;
    case IT_EDGE: s.edge += dir * step * 5; break;
    case IT_CONTRAST: s.contrast += dir * step; break;
    case IT_SHAPE: s.shape += dir * step; break;
    case IT_EXPOSURE: s.exposure += dir * step; break;
    case IT_GAIN:  // 0 = auto, otherwise 1.0x and up
        if (s.gain == 0) s.gain = dir > 0 ? 10 : 0;
        else if (dir < 0 && s.gain <= 10) s.gain = 0;
        else s.gain += dir * step;
        break;
    case IT_AX: s.roiX += dir * step * 8; break;
    case IT_AY: s.roiY += dir * step * 8; break;
    case IT_AW: s.roiW += dir * step * 8; break;
    case IT_AH: s.roiH += dir * step * 8; break;
    default: return;
    }
    shared_.set(s);
    // the list of rows changes with the mode: keep the cursor on a valid row
    int n = (int)visibleItems(shared_.get().mode).size();
    if (sel_ >= n) sel_ = n - 1;
}

// "OK" on the selected row. Saving needs two presses within three seconds so a stray key or a noisy
// input cannot overwrite the saved settings by accident.
void Ui::activate() {
    std::vector<int> items = visibleItems(shared_.get().mode);
    if (sel_ < 0 || sel_ >= (int)items.size()) return;
    if (items[sel_] == IT_SAVE) {
        long long t = nowMs();
        if (saveConfirmUntil_ > t) {
            message_ = shared_.get().save(confPath_.c_str()) ? "Saved" : "Save FAILED";
            messageUntil_ = t + 1500;
            saveConfirmUntil_ = 0;
            menu_ = false;
        } else {
            saveConfirmUntil_ = t + 3000;
        }
    } else if (items[sel_] == IT_DISCARD) {
        shared_.set(backup_);
        message_ = "Changes discarded";
        messageUntil_ = nowMs() + 1500;
        saveConfirmUntil_ = 0;
        menu_ = false;
    }
}

void Ui::onKey(Key k) {
    if (!menu_) {
        if (k == K_OK) enterMenu();
        return;
    }
    int n = (int)visibleItems(shared_.get().mode).size();
    switch (k) {
    case K_UP: sel_ = (sel_ + n - 1) % n; saveConfirmUntil_ = 0; break;
    case K_DOWN: sel_ = (sel_ + 1) % n; saveConfirmUntil_ = 0; break;
    case K_LEFT: changeValue(-1); break;
    case K_RIGHT: changeValue(+1); break;
    case K_ADJ: fast_ = !fast_; break;
    case K_OK: activate(); break;
    default: break;
    }
}

void Ui::onTouch(const TouchEvent& e) {
    if (e.down) {
        lastTouchX_ = e.x;
        lastTouchY_ = e.y;
        lastTouchAt_ = nowMs();
    }
    if (!e.down) return;
    if (!menu_) {
        if (!e.repeat) enterMenu();
        return;
    }
    if (e.y < kHeaderH) {  // header
        if (!e.repeat) fast_ = !fast_;
        return;
    }
    if (e.y < kRowTop) return;  // the camera picture between the header and the list

    std::vector<int> items = visibleItems(shared_.get().mode);
    // a tap below the list counts as a tap on its last row, down to the bottom edge
    int rel = std::min((e.y - kRowTop) / kRowH, kRows - 1);
    int n = (int)items.size();
    int row = ((firstRow() + rel) % n + n) % n;

    if (isValueItem(items[row]) && (e.x < 60 || e.x > 180)) {  // the < and > ends of a value row
        if (sel_ != row) saveConfirmUntil_ = 0;
        sel_ = row;
        changeValue(e.x < 60 ? -1 : +1);
        return;
    }
    if (e.repeat) return;
    if (sel_ != row) saveConfirmUntil_ = 0;
    sel_ = row;
    if (!isValueItem(items[row])) activate();
}

static void text(cv::Mat& img, const std::string& s, int x, int y, double scale, const cv::Scalar& c,
                 int thick = 1) {
    cv::putText(img, s, cv::Point(x, y), cv::FONT_HERSHEY_SIMPLEX, scale, c, thick, cv::LINE_AA);
}

// Blend a colour over part of the picture so text stays readable while the camera picture shows through.
static void shade(cv::Mat& img, const cv::Rect& r, const cv::Scalar& color, double alpha) {
    cv::Rect rr = r & cv::Rect(0, 0, img.cols, img.rows);
    if (rr.width <= 0 || rr.height <= 0) return;
    cv::Mat part = img(rr);
    cv::Mat fill(part.size(), part.type(), color);
    cv::addWeighted(fill, alpha, part, 1.0 - alpha, 0.0, part);
}

// Outline that follows the rounded corners of the LCD, drawn at the very edge of the screen.
static void roundBorder(cv::Mat& img, const cv::Scalar& color, int thick) {
    const int rad = 26;
    int w = img.cols, h = img.rows, m = thick / 2;  // centre line of the stroke
    int x0 = m, y0 = m, x1 = w - 1 - m, y1 = h - 1 - m;
    cv::line(img, cv::Point(x0 + rad, y0), cv::Point(x1 - rad, y0), color, thick, cv::LINE_AA);
    cv::line(img, cv::Point(x0 + rad, y1), cv::Point(x1 - rad, y1), color, thick, cv::LINE_AA);
    cv::line(img, cv::Point(x0, y0 + rad), cv::Point(x0, y1 - rad), color, thick, cv::LINE_AA);
    cv::line(img, cv::Point(x1, y0 + rad), cv::Point(x1, y1 - rad), color, thick, cv::LINE_AA);
    cv::ellipse(img, cv::Point(x0 + rad, y0 + rad), cv::Size(rad, rad), 0, 180, 270, color, thick, cv::LINE_AA);
    cv::ellipse(img, cv::Point(x1 - rad, y0 + rad), cv::Size(rad, rad), 0, 270, 360, color, thick, cv::LINE_AA);
    cv::ellipse(img, cv::Point(x0 + rad, y1 - rad), cv::Size(rad, rad), 0, 90, 180, color, thick, cv::LINE_AA);
    cv::ellipse(img, cv::Point(x1 - rad, y1 - rad), cv::Size(rad, rad), 0, 0, 90, color, thick, cv::LINE_AA);
}

// The camera picture fills the whole screen; the menu and results are laid over it. The picture is scaled to the
// screen height, so it is wider than the screen: the visible window follows the detection area.
void Ui::renderScreen(cv::Mat& out, const Snapshot& snap) {
    out.create(284, 240, CV_8UC3);
    out.setTo(cv::Scalar(24, 24, 24));
    Settings s = shared_.get();
    std::vector<int> items = visibleItems(s.mode);
    char b[64];

    double sc = 0;  // preview pixels per camera pixel
    int panX = 0;   // left edge of the visible window inside the preview
    if (!snap.preview.empty()) {
        sc = snap.preview.rows / 480.0;
        int centre = cvRound((s.roiX + s.roiW / 2) * sc);
        panX = std::max(0, std::min(centre - 120, snap.preview.cols - 240));
        cv::Rect r(panX, 0, std::min(240, snap.preview.cols - panX), std::min(284, snap.preview.rows));
        snap.preview(r).copyTo(out(cv::Rect(0, 0, r.width, r.height)));
        if (menu_ || snap.result >= 0)
            for (size_t i = 0; i < snap.circles.size(); ++i)
                cv::circle(out, cv::Point(cvRound(snap.circles[i][0]) - panX, cvRound(snap.circles[i][1])),
                           cvRound(snap.circles[i][2]), cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
    }

    // detection area: only what is inside the frame is looked at
    bool areaRow = menu_ && sel_ < (int)items.size() && items[sel_] >= IT_AX && items[sel_] <= IT_AH;
    if (sc > 0 && (s.roiX != 0 || s.roiY != 0 || s.roiW != 640 || s.roiH != 480 || areaRow)) {
        cv::Rect ra(cvRound(s.roiX * sc) - panX, cvRound(s.roiY * sc), cvRound(s.roiW * sc), cvRound(s.roiH * sc));
        cv::rectangle(out, ra, cv::Scalar(0, 200, 255), 1);
    }

    if (!menu_) {
        // clean result screen: the verdict is the colour of an outline around the whole screen (blue = front/OK,
        // red = back/NG, yellow = measuring, grey = nothing yet), plus a small text low on the screen.
        const char* label = "READY";
        cv::Scalar col(120, 120, 120);
        if (snap.result == 1) { label = "OK"; col = cv::Scalar(230, 130, 30); }
        else if (snap.result == 0) { label = "NG"; col = cv::Scalar(40, 40, 230); }
        else if (snap.result == 2) { label = "ERR"; col = cv::Scalar(0, 140, 240); }
        if (snap.busy) { label = "..."; col = cv::Scalar(0, 220, 255); }
        roundBorder(out, col, snap.result >= 0 || snap.busy ? 7 : 3);

        bool haveCount = (snap.result == 0 || snap.result == 1) && !snap.busy;
        if (haveCount) snprintf(b, sizeof(b), "%s  %d/%d", label, snap.count, s.target);
        else snprintf(b, sizeof(b), "%s", label);
        int base = 0;
        cv::Size ts = cv::getTextSize(b, cv::FONT_HERSHEY_SIMPLEX, 0.7, 2, &base);
        shade(out, cv::Rect(30, 208, 180, snap.held && !snap.busy ? 46 : 32), cv::Scalar(0, 0, 0), 0.55);
        text(out, b, (240 - ts.width) / 2, 232, 0.7, cv::Scalar(255, 255, 255), 2);
        if (snap.held && !snap.busy) {  // proof that a trigger arrived: its number and the time to the result
            snprintf(b, sizeof(b), "#%d   %.0f ms", snap.trigNo, snap.totalMs);
            cv::Size cs = cv::getTextSize(b, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &base);
            text(out, b, (240 - cs.width) / 2, 249, 0.5, cv::Scalar(225, 225, 225));
        }
        if (!snap.held) {  // hint only until the first trigger
            text(out, touchAvailable_ ? "tap or OK for menu" : "press OK for menu", touchAvailable_ ? 46 : 50, 252, 0.42,
                 cv::Scalar(215, 215, 215));
        }
        if (nowMs() < messageUntil_) {
            shade(out, cv::Rect(0, 150, 240, 30), cv::Scalar(0, 0, 0), 0.75);
            text(out, message_, 18, 172, 0.7, cv::Scalar(0, 255, 255), 2);
        }
        return;
    }

    // menu, laid over the picture: header at the top, a scrolling window of kRows lines at the bottom
    shade(out, cv::Rect(0, 0, 240, kHeaderH), cv::Scalar(0, 0, 0), 0.6);

    // live verdict for what the camera sees right now: front (OK, count matches) or back (NG).
    // The text is measured and centred inside its coloured block so it can never spill out of it.
    bool liveOk = (snap.count == s.target);
    cv::Rect badge(128, 5, 86, 22);
    cv::rectangle(out, badge, liveOk ? cv::Scalar(40, 170, 40) : cv::Scalar(40, 40, 200), -1);
    snprintf(b, sizeof(b), "%s %d/%d", liveOk ? "OK" : "NG", snap.count, s.target);
    int base = 0;
    cv::Size ts = cv::getTextSize(b, cv::FONT_HERSHEY_SIMPLEX, 0.55, 2, &base);
    text(out, b, badge.x + (badge.width - ts.width) / 2, badge.y + (badge.height + ts.height) / 2,
         0.55, cv::Scalar(255, 255, 255), 2);
    // "MENU" sits on the same baseline as the verdict text so the two line up
    text(out, fast_ ? "MENU x10" : "MENU", 26, badge.y + (badge.height + ts.height) / 2, 0.55, cv::Scalar(0, 255, 255), 2);

    shade(out, cv::Rect(0, kRowTop - 2, 240, kRows * kRowH + 4), cv::Scalar(0, 0, 0), 0.62);
    int first = firstRow();
    for (int row = 0; row < kRows; ++row) {
        int n = (int)items.size();
        int i = ((first + row) % n + n) % n;
        int y = kRowTop + row * kRowH;
        bool on = (i == sel_);
        bool pending = (items[i] == IT_SAVE && saveConfirmUntil_ > nowMs());
        if (on) shade(out, cv::Rect(0, y, 240, kRowH), pending ? cv::Scalar(30, 120, 220) : cv::Scalar(200, 110, 30), 0.85);
        cv::Scalar fg = on ? cv::Scalar(255, 255, 255) : cv::Scalar(225, 225, 225);
        if (on && isValueItem(items[i])) {  // - and + only on the selected row
            // drawn as lines so they sit exactly in the middle of the row
            const cv::Scalar sign(120, 210, 255);
            int cy = y + kRowH / 2;
            cv::line(out, cv::Point(10, cy), cv::Point(24, cy), sign, 3, cv::LINE_AA);
            cv::line(out, cv::Point(206, cy), cv::Point(220, cy), sign, 3, cv::LINE_AA);
            cv::line(out, cv::Point(213, cy - 7), cv::Point(213, cy + 7), sign, 3, cv::LINE_AA);
        }
        text(out, itemText(items[i], s), 32, y + 24, 0.5, fg);
    }
}

void Ui::render(cv::Mat& out, const Snapshot& snap) {
    if (snap.preview.empty()) {  // no camera picture yet: logo and a status message on a white screen
        out.create(284, 240, CV_8UC3);
        out.setTo(cv::Scalar(255, 255, 255));
        // logo, centred in the upper part (the LCD has rounded corners, so nothing sits near the edges)
        cv::Mat logo(kLogoH, kLogoW, CV_8UC3, const_cast<unsigned char*>(kLogoBgr));
        logo.copyTo(out(cv::Rect((240 - kLogoW) / 2, 62, kLogoW, kLogoH)));

        std::string msg = snap.status.empty() ? "Starting" : snap.status;
        bool error = msg.find("NOT FOUND") != std::string::npos || msg.find("FAILED") != std::string::npos;
        // animated dots after a plain "Starting ..." message, so it is clear the system is alive
        bool animate = !error && msg.size() > 0 && msg[msg.size() - 1] != '.';
        if (animate) msg += std::string((nowMs() / 450) % 4, '.');

        const cv::Scalar blue(148, 83, 11), grey(90, 90, 90), red(40, 40, 220);
        int y = 190;
        size_t pos = 0;
        while (pos <= msg.size()) {
            size_t nl = msg.find('\n', pos);
            std::string line = msg.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            bool first = (y == 190);
            double scale = first ? 0.62 : 0.48;
            int base = 0;
            // measure without the animated dots so the text does not wobble sideways
            std::string measure = line;
            while (animate && first && !measure.empty() && measure[measure.size() - 1] == '.') measure.erase(measure.size() - 1);
            cv::Size ts = cv::getTextSize(measure + (animate && first ? "..." : ""), cv::FONT_HERSHEY_SIMPLEX, scale, 1, &base);
            text(out, line, std::max(14, (240 - ts.width) / 2), y, scale, first ? (error ? red : blue) : grey, 1);
            y += 26;
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        return;
    }
    renderScreen(out, snap);
    // touch debugging: a magenta cross and ring where the controller reports the finger, kept for 1.5 s
    if (touchDebug_ && nowMs() - lastTouchAt_ < 1500 && lastTouchAt_ > 0) {
        cv::Point p(lastTouchX_, lastTouchY_);
        cv::circle(out, p, 14, cv::Scalar(255, 0, 255), 2, cv::LINE_AA);
        cv::line(out, cv::Point(p.x - 20, p.y), cv::Point(p.x + 20, p.y), cv::Scalar(255, 0, 255), 1);
        cv::line(out, cv::Point(p.x, p.y - 20), cv::Point(p.x, p.y + 20), cv::Scalar(255, 0, 255), 1);
        char b[32];
        snprintf(b, sizeof(b), "%d,%d", lastTouchX_, lastTouchY_);
        text(out, b, std::max(2, std::min(170, p.x - 24)), std::max(14, p.y - 20), 0.45, cv::Scalar(255, 0, 255), 1);
    }
}
