#include "ui.h"

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

const int kRows = 4;      // menu lines visible at once
const int kRowTop = 184;  // first menu row, pixels
const int kRowH = 20;

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
    case IT_MODE: snprintf(b, sizeof(b), "Mode      %s", s.mode == 1 ? "Dark hole" : "Circle"); break;
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
    int n = (int)visibleItems(shared_.get().mode).size();
    int first = sel_ - kRows / 2;
    if (first < 0) first = 0;
    if (first > n - kRows) first = n - kRows;
    return first;
}

void Ui::enterMenu() {
    menu_ = true;
    sel_ = 0;
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
    if (!e.down) return;
    if (!menu_) {
        if (!e.repeat) enterMenu();
        return;
    }
    if (e.y < 24) {  // header
        if (!e.repeat) fast_ = !fast_;
        return;
    }
    if (e.y < kRowTop || e.y >= kRowTop + kRows * kRowH) return;

    std::vector<int> items = visibleItems(shared_.get().mode);
    int row = firstRow() + (e.y - kRowTop) / kRowH;
    if (row < 0 || row >= (int)items.size()) return;

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

void Ui::render(cv::Mat& out, const Snapshot& snap) {
    out.create(284, 240, CV_8UC3);
    out.setTo(cv::Scalar(24, 24, 24));

    if (!snap.preview.empty()) {
        cv::Rect r(0, 0, std::min(240, snap.preview.cols), std::min(180, snap.preview.rows));
        snap.preview(r).copyTo(out(r));
        if (menu_ || snap.result >= 0)
            for (size_t i = 0; i < snap.circles.size(); ++i)
                cv::circle(out, cv::Point(cvRound(snap.circles[i][0]), cvRound(snap.circles[i][1])),
                           cvRound(snap.circles[i][2]), cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
    }

    Settings s = shared_.get();
    std::vector<int> items = visibleItems(s.mode);
    char b[64];

    // detection area: only what is inside the frame is looked at (preview is 0.375 of the camera picture)
    bool areaRow = menu_ && sel_ < (int)items.size() && items[sel_] >= IT_AX && items[sel_] <= IT_AH;
    if (s.roiX != 0 || s.roiY != 0 || s.roiW != 640 || s.roiH != 480 || areaRow) {
        cv::Rect ra(cvRound(s.roiX * 0.375), cvRound(s.roiY * 0.375), cvRound(s.roiW * 0.375), cvRound(s.roiH * 0.375));
        ra &= cv::Rect(0, 0, 240, 180);
        cv::rectangle(out, ra, cv::Scalar(0, 200, 255), 1);
    }

    if (!menu_) {
        const char* label = "--";
        cv::Scalar col(160, 160, 160);
        if (snap.result == 1) { label = "OK"; col = cv::Scalar(60, 220, 60); }
        else if (snap.result == 0) { label = "NG"; col = cv::Scalar(60, 60, 240); }
        else if (snap.result == 2) { label = "ERR"; col = cv::Scalar(0, 140, 255); }
        // the LCD has rounded corners: keep everything about 16 px away from the left, right and bottom edges
        text(out, label, 18, 244, 2.0, col, 4);
        snprintf(b, sizeof(b), "found %d/%d", snap.count, s.target);
        text(out, b, 108, 208, 0.5, cv::Scalar(230, 230, 230));
        snprintf(b, sizeof(b), "%.0ffps %.1fms", snap.fps, snap.detectMs);
        text(out, b, 108, 230, 0.45, cv::Scalar(180, 180, 180));
        text(out, "tap or OK for menu", 46, 268, 0.42, cv::Scalar(150, 150, 150));
        if (nowMs() < messageUntil_) {
            cv::rectangle(out, cv::Rect(0, 150, 240, 30), cv::Scalar(0, 0, 0), -1);
            text(out, message_, 18, 172, 0.7, cv::Scalar(0, 255, 255), 2);
        }
        return;
    }

    // menu: scrolling window of kRows lines under the live preview
    cv::rectangle(out, cv::Rect(0, 0, 240, 24), cv::Scalar(0, 0, 0), -1);
    text(out, fast_ ? "MENU  [x10]" : "MENU", 22, 18, 0.55, cv::Scalar(0, 255, 255));
    text(out, "tap = x10", 140, 17, 0.4, cv::Scalar(150, 150, 150));

    // live verdict for what the camera sees right now: front (OK, count matches) or back (NG)
    bool liveOk = (snap.count == s.target);
    cv::Rect badge(150, 30, 70, 44);
    cv::rectangle(out, badge, liveOk ? cv::Scalar(40, 170, 40) : cv::Scalar(40, 40, 200), -1);
    cv::rectangle(out, badge, cv::Scalar(255, 255, 255), 1);
    text(out, liveOk ? "OK" : "NG", badge.x + 10, badge.y + 28, 1.0, cv::Scalar(255, 255, 255), 2);
    snprintf(b, sizeof(b), "%d/%d", snap.count, s.target);
    text(out, b, badge.x + 8, badge.y + 41, 0.4, cv::Scalar(255, 255, 255));
    int first = firstRow();
    for (int row = 0; row < kRows; ++row) {
        int i = first + row;
        if (i >= (int)items.size()) break;
        int y = kRowTop + row * kRowH;
        bool on = (i == sel_);
        bool pending = (items[i] == IT_SAVE && saveConfirmUntil_ > nowMs());
        if (on) cv::rectangle(out, cv::Rect(0, y, 240, kRowH), pending ? cv::Scalar(30, 120, 220) : cv::Scalar(160, 90, 20), -1);
        cv::Scalar fg = on ? cv::Scalar(255, 255, 255) : cv::Scalar(210, 210, 210);
        if (isValueItem(items[i])) {
            text(out, "<", 14, y + 15, 0.55, cv::Scalar(120, 200, 255), 2);
            text(out, ">", 212, y + 15, 0.55, cv::Scalar(120, 200, 255), 2);
        }
        text(out, itemText(items[i], s), 32, y + 15, 0.5, fg);
    }
    snprintf(b, sizeof(b), "found %d  (%.1f ms)", snap.count, snap.detectMs);
    text(out, b, 66, 276, 0.42, cv::Scalar(150, 150, 150));
}
