#include "ui.h"

#include <stdio.h>
#include <time.h>
#include <algorithm>
#include <opencv2/imgproc/imgproc.hpp>

static const int kItems = 8;
static const int kRows = 4;  // menu lines visible at once

static long long nowMs() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

Ui::Ui(SharedSettings& shared, const std::string& confPath)
    : shared_(shared), confPath_(confPath), menu_(false), sel_(0), fast_(false), messageUntil_(0) {}

std::string Ui::itemText(int i, const Settings& s) const {
    char b[48];
    switch (i) {
    case 0: snprintf(b, sizeof(b), "Circles   %d", s.target); break;
    case 1: snprintf(b, sizeof(b), "Min dia   %d px", s.minDia); break;
    case 2: snprintf(b, sizeof(b), "Max dia   %d px", s.maxDia); break;
    case 3: snprintf(b, sizeof(b), "Roundness %d", s.roundness); break;
    case 4: snprintf(b, sizeof(b), "Edge      %d", s.edge); break;
    case 5:
        if (s.exposure == 0) snprintf(b, sizeof(b), "Exposure  auto");
        else snprintf(b, sizeof(b), "Exposure  %.1f ms", s.exposure / 10.0);
        break;
    case 6: snprintf(b, sizeof(b), "[ Save and exit ]"); break;
    default: snprintf(b, sizeof(b), "[ Discard changes ]"); break;
    }
    return b;
}

void Ui::changeValue(int dir) {
    Settings s = shared_.get();
    int step = fast_ ? 10 : 1;
    switch (sel_) {
    case 0: s.target += dir * step; break;
    case 1: s.minDia += dir * step * 2; break;
    case 2: s.maxDia += dir * step * 2; break;
    case 3: s.roundness += dir * step; break;
    case 4: s.edge += dir * step * 5; break;
    case 5: s.exposure += dir * step; break;
    default: return;
    }
    shared_.set(s);
}

void Ui::onKey(Key k) {
    if (!menu_) {
        if (k == K_OK) {
            menu_ = true;
            sel_ = 0;
            fast_ = false;
            backup_ = shared_.get();
        }
        return;
    }
    switch (k) {
    case K_UP: sel_ = (sel_ + kItems - 1) % kItems; break;
    case K_DOWN: sel_ = (sel_ + 1) % kItems; break;
    case K_LEFT: changeValue(-1); break;
    case K_RIGHT: changeValue(+1); break;
    case K_ADJ: fast_ = !fast_; break;
    case K_OK:
        if (sel_ == 6) {
            message_ = shared_.get().save(confPath_.c_str()) ? "Saved" : "Save FAILED";
            messageUntil_ = nowMs() + 1500;
            menu_ = false;
        } else if (sel_ == 7) {
            shared_.set(backup_);
            message_ = "Changes discarded";
            messageUntil_ = nowMs() + 1500;
            menu_ = false;
        }
        break;
    default: break;
    }
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
    char b[64];

    if (!menu_) {
        const char* label = "--";
        cv::Scalar col(160, 160, 160);
        if (snap.result == 1) { label = "OK"; col = cv::Scalar(60, 220, 60); }
        else if (snap.result == 0) { label = "NG"; col = cv::Scalar(60, 60, 240); }
        else if (snap.result == 2) { label = "ERR"; col = cv::Scalar(0, 140, 255); }
        text(out, label, 10, 245, 2.2, col, 4);
        snprintf(b, sizeof(b), "found %d / %d", snap.count, s.target);
        text(out, b, 120, 205, 0.55, cv::Scalar(230, 230, 230));
        snprintf(b, sizeof(b), "%.0f fps  %.1f ms", snap.fps, snap.detectMs);
        text(out, b, 120, 228, 0.5, cv::Scalar(180, 180, 180));
        text(out, "press OK for menu", 10, 276, 0.45, cv::Scalar(150, 150, 150));
        if (nowMs() < messageUntil_) {
            cv::rectangle(out, cv::Rect(0, 150, 240, 30), cv::Scalar(0, 0, 0), -1);
            text(out, message_, 10, 172, 0.7, cv::Scalar(0, 255, 255), 2);
        }
        return;
    }

    // menu: scrolling window of kRows lines under the live preview
    cv::rectangle(out, cv::Rect(0, 0, 240, 22), cv::Scalar(0, 0, 0), -1);
    text(out, fast_ ? "MENU  [x10]" : "MENU", 6, 16, 0.55, cv::Scalar(0, 255, 255));
    int first = sel_ - kRows / 2;
    if (first < 0) first = 0;
    if (first > kItems - kRows) first = kItems - kRows;
    for (int row = 0; row < kRows; ++row) {
        int i = first + row;
        int y = 184 + row * 20;
        bool on = (i == sel_);
        if (on) cv::rectangle(out, cv::Rect(0, y, 240, 20), cv::Scalar(160, 90, 20), -1);
        text(out, itemText(i, s), 8, y + 15, 0.5, on ? cv::Scalar(255, 255, 255) : cv::Scalar(210, 210, 210));
    }
    snprintf(b, sizeof(b), "found %d  (%.1f ms)", snap.count, snap.detectMs);
    text(out, b, 6, 280, 0.42, cv::Scalar(150, 150, 150));
}
