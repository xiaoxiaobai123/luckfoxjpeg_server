#include "ui.h"
#include "logo.h"
#include "font_cn.h"

#include <stdio.h>
#include <time.h>
#include <algorithm>
#include <math.h>
#include <string>
#include <opencv2/imgproc/imgproc.hpp>

namespace {

enum Item {
    IT_MODE, IT_TARGET, IT_MINDIA, IT_MAXDIA,
    IT_ROUND, IT_EDGE,        // circle mode only
    IT_CONTRAST, IT_SHAPE,    // dark-hole mode only
    IT_EXPOSURE, IT_GAIN, IT_AX, IT_AY, IT_AW, IT_AH,
    IT_LANG, IT_SAVE, IT_DISCARD
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
    v.push_back(IT_LANG);
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
    case IT_LANG: snprintf(b, sizeof(b), "Language  %s", s.lang ? "Chinese" : "English"); break;
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
    case IT_LANG: s.lang = s.lang ? 0 : 1; break;
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

// ---- Chinese text: glyph bitmaps generated at build time (tools/genfont.py) from the strings in this file ----

// Walks a UTF-8 string, drawing it (img != NULL) or only measuring it. cy is the vertical middle of the line.
static int cnWalk(cv::Mat* img, const std::string& str, int x, int cy, const cv::Scalar& color, int size, bool outline) {
    const bool big = size >= 22;
    const CnGlyph* tab = big ? kCn22 : kCn16;
    const int n = big ? kCn22N : kCn16N;
    const unsigned char* bits = big ? kCn22Bits : kCn16Bits;
    const int baseline = cy + (big ? 8 : 6);
    int pen = x;
    for (size_t i = 0; i < str.size();) {
        unsigned char c = (unsigned char)str[i];
        unsigned cp = c;
        int len = 1;
        if (c >= 0xE0 && i + 2 < str.size()) {
            cp = ((c & 0x0F) << 12) | (((unsigned char)str[i + 1] & 0x3F) << 6) | ((unsigned char)str[i + 2] & 0x3F);
            len = 3;
        } else if (c >= 0xC0 && i + 1 < str.size()) {
            cp = ((c & 0x1F) << 6) | ((unsigned char)str[i + 1] & 0x3F);
            len = 2;
        }
        i += len;
        int lo = 0, hi = n - 1, at = -1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (tab[mid].cp == cp) { at = mid; break; }
            if (tab[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
        }
        if (at < 0) continue;
        const CnGlyph& g = tab[at];
        if (img && g.w && g.h) {
            static const int off[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, 1}, {-1, 1}, {1, -1}};
            for (int pass = outline ? 0 : 1; pass < 2; ++pass) {
                for (int d = 0; d < (pass == 0 ? 8 : 1); ++d) {
                    int ox = pass == 0 ? off[d][0] : 0, oy = pass == 0 ? off[d][1] : 0;
                    for (int yy = 0; yy < g.h; ++yy) {
                        int py = baseline + g.by + yy + oy;
                        if (py < 0 || py >= img->rows) continue;
                        cv::Vec3b* row = img->ptr<cv::Vec3b>(py);
                        for (int xx = 0; xx < g.w; ++xx) {
                            int a = bits[g.off + yy * g.w + xx];
                            int px = pen + g.bx + xx + ox;
                            if (a == 0 || px < 0 || px >= img->cols) continue;
                            for (int ch = 0; ch < 3; ++ch) {
                                int fg = pass == 0 ? 0 : (int)color[ch];
                                row[px][ch] = (uchar)((row[px][ch] * (255 - a) + fg * a) / 255);
                            }
                        }
                    }
                }
            }
        }
        pen += g.adv;
    }
    return pen - x;
}

static int cnDraw(cv::Mat& img, const std::string& s, int x, int cy, const cv::Scalar& c, int size, bool outline) {
    return cnWalk(&img, s, x, cy, c, size, outline);
}

static int cnWidth(const std::string& s, int size) { return cnWalk(NULL, s, 0, 0, cv::Scalar(), size, false); }

static void textOutline(cv::Mat& img, const std::string& s, int x, int y, double scale, const cv::Scalar& c, int thick) {
    text(img, s, x, y, scale, cv::Scalar(0, 0, 0), thick + 3);
    text(img, s, x, y, scale, c, thick);
}

// Menu row in Chinese: label and value are drawn separately so the values line up in a column.
static void itemPartsZh(int id, const Settings& s, bool savePending, std::string& label, std::string& value) {
    char b[48];
    value.clear();
    switch (id) {
    case IT_MODE: label = "模式"; value = s.mode == 1 ? "暗孔" : "圆形"; break;
    case IT_TARGET: label = "数量"; snprintf(b, sizeof(b), "%d", s.target); value = b; break;
    case IT_MINDIA: label = "最小直径"; snprintf(b, sizeof(b), "%d px", s.minDia); value = b; break;
    case IT_MAXDIA: label = "最大直径"; snprintf(b, sizeof(b), "%d px", s.maxDia); value = b; break;
    case IT_ROUND: label = "圆度"; snprintf(b, sizeof(b), "%d", s.roundness); value = b; break;
    case IT_EDGE: label = "边缘"; snprintf(b, sizeof(b), "%d", s.edge); value = b; break;
    case IT_CONTRAST: label = "对比度"; snprintf(b, sizeof(b), "%d", s.contrast); value = b; break;
    case IT_SHAPE: label = "形状"; snprintf(b, sizeof(b), "%d %%", s.shape); value = b; break;
    case IT_EXPOSURE:
        label = "曝光";
        if (s.exposure == 0) value = "自动";
        else { snprintf(b, sizeof(b), "%.1f ms", s.exposure / 10.0); value = b; }
        break;
    case IT_GAIN:
        label = "增益";
        if (s.gain == 0) value = "自动";
        else { snprintf(b, sizeof(b), "%.1fx", s.gain / 10.0); value = b; }
        break;
    case IT_AX: label = "区域 X"; snprintf(b, sizeof(b), "%d", s.roiX); value = b; break;
    case IT_AY: label = "区域 Y"; snprintf(b, sizeof(b), "%d", s.roiY); value = b; break;
    case IT_AW: label = "区域 宽"; snprintf(b, sizeof(b), "%d", s.roiW); value = b; break;
    case IT_AH: label = "区域 高"; snprintf(b, sizeof(b), "%d", s.roiH); value = b; break;
    case IT_LANG: label = "语言"; value = "中文"; break;
    case IT_SAVE: label = savePending ? "[ 再按一次保存 ]" : "[ 保存并退出 ]"; break;
    default: label = "[ 放弃修改 ]"; break;
    }
}

static std::string messageZh(const std::string& m) {
    if (m == "Saved") return "已保存";
    if (m == "Save FAILED") return "保存失败";
    if (m == "Changes discarded") return "已放弃修改";
    return m;
}

// Outline that follows the rounded corners of the LCD, drawn at the very edge of the screen.
static void roundBorder(cv::Mat& img, const cv::Scalar& color, int thick) {
    const int rad = 30;
    const int inset = 3;  // the very edge of the panel may be hidden by the bezel
    int w = img.cols, h = img.rows, m = inset + thick / 2;  // centre line of the stroke
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

// Edge light like a consumer device: a small, soft glow just inside the outline; fade scales its strength.
static void glowBorder(cv::Mat& out, const cv::Scalar& color, int thick, double fade) {
    if (fade <= 0.01) { roundBorder(out, color, thick); return; }
    cv::Mat mask = cv::Mat::zeros(out.size(), CV_8UC1);
    roundBorder(mask, cv::Scalar(255), 6);
    cv::GaussianBlur(mask, mask, cv::Size(0, 0), 3.0);
    for (int y = 0; y < out.rows; ++y) {
        const uchar* m = mask.ptr<uchar>(y);
        cv::Vec3b* o = out.ptr<cv::Vec3b>(y);
        for (int x = 0; x < out.cols; ++x) {
            if (!m[x]) continue;
            double g = m[x] / 255.0 * 1.1 * fade;
            for (int c = 0; c < 3; ++c) o[x][c] = (uchar)std::min(255.0, o[x][c] + color[c] * g);
        }
    }
    roundBorder(out, color, thick);
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
    int yOff = 0;   // top of the preview picture on the screen
    // run screen: the detection area (the whole picture when none is set) scaled up to fill the screen, like a
    // hardware ROI. Menu: the whole picture, scaled to the screen width, so the area can be set against it.
    const bool zoomed = !menu_ && !snap.roiView.empty();
    if (zoomed) {
        int x0 = (240 - snap.roiView.cols) / 2, y0 = (284 - snap.roiView.rows) / 2;
        cv::Rect dst(x0, y0, snap.roiView.cols, snap.roiView.rows);
        dst &= cv::Rect(0, 0, 240, 284);
        snap.roiView(cv::Rect(0, 0, dst.width, dst.height)).copyTo(out(dst));
        if (snap.result >= 0)
            for (size_t i = 0; i < snap.roiCircles.size(); ++i)
                cv::circle(out, cv::Point(cvRound(snap.roiCircles[i][0]) + x0, cvRound(snap.roiCircles[i][1]) + y0),
                           cvRound(snap.roiCircles[i][2]), cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
    } else if (!snap.preview.empty()) {
        sc = snap.preview.cols / 640.0;
        yOff = kHeaderH + 4;
        cv::Rect r(0, yOff, std::min(240, snap.preview.cols), std::min(284 - yOff, snap.preview.rows));
        snap.preview(cv::Rect(0, 0, r.width, r.height)).copyTo(out(r));
        if (menu_ || snap.result >= 0)
            for (size_t i = 0; i < snap.circles.size(); ++i)
                cv::circle(out, cv::Point(cvRound(snap.circles[i][0]), cvRound(snap.circles[i][1]) + yOff),
                           cvRound(snap.circles[i][2]), cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
    }

    // detection area: only what is inside the frame is looked at (menu only: the run screen shows just that area)
    bool areaRow = menu_ && sel_ < (int)items.size() && items[sel_] >= IT_AX && items[sel_] <= IT_AH;
    if (!zoomed && sc > 0 && (s.roiX != 0 || s.roiY != 0 || s.roiW != 640 || s.roiH != 480 || areaRow)) {
        cv::Rect ra(cvRound(s.roiX * sc), cvRound(s.roiY * sc) + yOff, cvRound(s.roiW * sc), cvRound(s.roiH * sc));
        cv::rectangle(out, ra, cv::Scalar(0, 200, 255), 1);
    }

    if (!menu_) {
        // clean result screen: the verdict is the colour of an outline around the whole screen (blue = front/OK,
        // red = back/NG, yellow = measuring, grey = nothing yet); the verdict word (lower left) and the count
        // (lower right) are drawn in the same colour, without a background.
        const bool zh = s.lang == 1;
        std::string label = zh ? "就绪" : "READY";
        cv::Scalar col(120, 120, 120), tcol(235, 235, 235);
        if (snap.result == 1) { label = "OK"; col = cv::Scalar(60, 200, 60); tcol = cv::Scalar(100, 235, 100); }
        else if (snap.result == 0) { label = "NG"; col = cv::Scalar(40, 40, 230); tcol = cv::Scalar(90, 90, 255); }
        else if (snap.result == 2) { label = zh ? "错误" : "ERR"; col = cv::Scalar(0, 140, 240); tcol = cv::Scalar(0, 175, 255); }
        if (snap.busy) { label = "..."; col = cv::Scalar(120, 120, 120); tcol = cv::Scalar(0, 220, 255); }
        // edge light: yellow while measuring, then it turns into the result colour (green = OK, red = NG) with a
        // gradual change, the glow fades out after a moment and a thin outline stays
        const long long now = nowMs();
        const cv::Scalar yellow(0, 220, 255);
        if (snap.trigNo != lastTrigNo_) {
            lastTrigNo_ = snap.trigNo;
            trigShownAt_ = now;
            if (!snap.busy) resultAt_ = now;
        }
        if (wasBusy_ && !snap.busy) resultAt_ = now;
        wasBusy_ = snap.busy;
        double fade = 1.0;
        if (snap.busy) {
            col = yellow;
        } else if (snap.result >= 0 && trigShownAt_ > 0) {
            double t = std::min(1.0, (now - resultAt_) / 300.0);  // yellow -> result colour
            col = yellow * (1.0 - t) + col * t;
            long long since = now - resultAt_;
            fade = since < 300 ? 1.0 : 1.0 - (since - 300) / 500.0;
        } else if (snap.result >= 0) {
            fade = 0;
        }
        if (snap.result >= 0 || snap.busy) glowBorder(out, col, 4, fade);
        else roundBorder(out, col, 3);

        bool haveCount = (snap.result == 0 || snap.result == 1) && !snap.busy;
        const int cy = 250;  // both on one line, a little above the outline
        int base = 0;
        if ((unsigned char)label[0] >= 0x80) {
            cnDraw(out, label, 34, cy, tcol, 22, true);
        } else {
            cv::Size ts = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.9, 2, &base);
            textOutline(out, label, 34, cy + ts.height / 2, 0.9, tcol, 2);
        }
        if (haveCount) {
            snprintf(b, sizeof(b), "%d/%d", snap.count, s.target);
            cv::Size cs = cv::getTextSize(b, cv::FONT_HERSHEY_SIMPLEX, 0.6, 2, &base);
            textOutline(out, b, 206 - cs.width, cy + cs.height / 2, 0.6, tcol, 2);
        }
        if (!snap.held) {  // hint only until the first trigger
            if (zh) {
                std::string hint = touchAvailable_ ? "点屏幕或按 OK 进入菜单" : "按 OK 进入菜单";
                cnDraw(out, hint, (240 - cnWidth(hint, 16)) / 2, 222, cv::Scalar(225, 225, 225), 16, true);
            } else {
                textOutline(out, touchAvailable_ ? "tap or OK for menu" : "press OK for menu", touchAvailable_ ? 46 : 50, 226,
                            0.42, cv::Scalar(225, 225, 225), 1);
            }
        }
        if (nowMs() < messageUntil_) {
            shade(out, cv::Rect(0, 150, 240, 30), cv::Scalar(0, 0, 0), 0.75);
            if (s.lang == 1) cnDraw(out, messageZh(message_), 18, 165, cv::Scalar(0, 255, 255), 22, false);
            else text(out, message_, 18, 172, 0.7, cv::Scalar(0, 255, 255), 2);
        }
        return;
    }

    // menu, laid over the picture: header at the top, a scrolling window of kRows lines at the bottom
    shade(out, cv::Rect(0, 0, 240, kHeaderH), cv::Scalar(0, 0, 0), 0.6);

    // live verdict for what the camera sees right now: front (OK, count matches) or back (NG).
    // The text is measured and centred inside its coloured block so it can never spill out of it.
    bool liveOk = (snap.count >= s.target);  // at least the target number of holes = front (OK)
    cv::Rect badge((240 - 86) / 2, 5, 86, 22);
    cv::rectangle(out, badge, liveOk ? cv::Scalar(40, 170, 40) : cv::Scalar(40, 40, 200), -1);
    snprintf(b, sizeof(b), "%s %d/%d", liveOk ? "OK" : "NG", snap.count, s.target);
    int base = 0;
    cv::Size ts = cv::getTextSize(b, cv::FONT_HERSHEY_SIMPLEX, 0.55, 2, &base);
    text(out, b, badge.x + (badge.width - ts.width) / 2, badge.y + (badge.height + ts.height) / 2,
         0.55, cv::Scalar(255, 255, 255), 2);

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
        if (s.lang == 1) {
            std::string lab, val;
            itemPartsZh(items[i], s, pending, lab, val);
            cnDraw(out, lab, 32, y + kRowH / 2, fg, 16, false);
            if (!val.empty()) cnDraw(out, val, 134, y + kRowH / 2, fg, 16, false);
        } else {
            text(out, itemText(items[i], s), 32, y + 24, 0.5, fg);
        }
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
