// Luckfox Pico (RV1106 / RV1103): camera -> circle detection -> PLC outputs, with a small LCD menu.
//
//   PLC trigger (input, rising edge) -> take a picture -> count circles
//   OK output = 1 when the count equals the target, NG output = 1 otherwise, ERR output = camera fault.
//   The outputs are cleared on the trigger edge and stay latched until the next trigger.
//   LCD + keys: OK opens the menu (circle count, diameter range, roundness, edge, exposure, save).
//
// Bench use without wiring:  --web 8080  then open http://<board-ip>:8080/ (camera, LCD picture, keys)
// Full option list: see printUsage().

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <pthread.h>
#include <dirent.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <algorithm>

extern "C" {
#include "rk_mpi_sys.h"
#include "rk_mpi_vi.h"
#include "rk_aiq_user_api2_sysctl.h"
#include "rk_aiq_user_api2_ae.h"
#include "rk_aiq_types.h"
}
#include "luckfox_mpi.h"
#include "gpio.h"
#include "settings.h"
#include "keys.h"
#include "display.h"
#include "ui.h"
#include "web.h"
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#define CAP_WIDTH  640
#define CAP_HEIGHT 480
#define WORK_W 320
#define WORK_H 240

struct Params {
    // GPIO numbers = bank * 32 + group * 8 + index (A=0..D=3). Defaults are the final wiring plan for the
    // Pico Mini (touch reset on GPIO 56, I2C3 on GPIO 58/59, LCD backlight wired straight to 3.3V).
    int pinTrigger = 55, pinOk = 54, pinNg = 57, pinErr = 4;
    int pinDc = 52, pinRst = 53, pinBl = -1;
    int keyPins[6] = {64, 65, 66, 67, 70, 69};  // OK, UP, DOWN, LEFT, RIGHT, ADJ
    std::string keyMode = "gpio";               // gpio | adc | none
    int adcChannel = 0;
    std::string touchDev;   // e.g. /dev/i2c-3 (CST816D at 0x15); empty = no touch
    int touchRst = -1;      // touch controller reset GPIO, -1 = none
    std::string waitFile;   // wait for this file to exist before using the camera (the autostart script creates it)
    int autoRestart = 0;    // camera not found at boot: restart the board by itself up to this many times (0 = never)
    bool touchDebug = false;  // mark touches on the LCD and log their coordinates
    std::string spidev = "/dev/spidev0.0";
    int spiHz = 40000000;
    bool noScreen = false;
    int webPort = 0;
    bool continuous = false;
    std::string conf = "/userdata/circle.conf";
    std::string imagePath;
    // command-line overrides of the saved settings (-1 = not given)
    int oTarget = -1, oMinDia = -1, oMaxDia = -1, oRound = -1, oEdge = -1, oExposure = -1;
    int oMode = -1, oContrast = -1, oShape = -1;
};

static Params g_p;
static SharedSettings g_shared;
static volatile bool g_run = true;
static volatile bool g_menuOpen = false;
static volatile int g_trigPending = 0;
static volatile double g_trigAt = 0;  // time of the last trigger edge, ms
static volatile int g_trigCount = 0;

static Gpio g_ioTrigger, g_ioOk, g_ioNg, g_ioErr;

static Snapshot g_snap;
static pthread_mutex_t g_snapLock = PTHREAD_MUTEX_INITIALIZER;
static rk_aiq_sys_ctx_t* g_aiq = NULL;

static void onSignal(int) { g_run = false; }

// A message for the LCD while there is no camera picture (start-up, or the camera is missing).
static pthread_mutex_t g_statusLock = PTHREAD_MUTEX_INITIALIZER;
static std::string g_status;

static void setStatus(const std::string& s) {
    pthread_mutex_lock(&g_statusLock);
    g_status = s;
    pthread_mutex_unlock(&g_statusLock);
}

static std::string getStatus() {
    pthread_mutex_lock(&g_statusLock);
    std::string s = g_status;
    pthread_mutex_unlock(&g_statusLock);
    return s;
}

// The camera sensor is detected by the kernel at boot: its driver binds to an I2C device. If the ribbon cable
// is loose, the probe fails and no driver is bound. Starting the camera stack then crashes, so check first.
// Returns false only when a known sensor device exists but none of them got a driver.
static bool sensorPresent() {
    static const char* names[] = {"sc3336", "sc4336", "sc530ai", "sc200ai", "sc231ai", "sc2336", "sc450ai",
                                  "mia1321", "imx415", "imx327", "gc2093", "gc4653", "ov5647", "os04a10", NULL};
    DIR* d = opendir("/sys/bus/i2c/devices");
    if (!d) return true;  // cannot tell, try anyway
    int bound = 0, unbound = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        std::string base = std::string("/sys/bus/i2c/devices/") + e->d_name;
        FILE* f = fopen((base + "/name").c_str(), "r");
        if (!f) continue;
        char nm[64] = {0};
        if (!fgets(nm, sizeof(nm), f)) nm[0] = 0;
        fclose(f);
        bool known = false;
        for (int i = 0; names[i]; ++i)
            if (strncmp(nm, names[i], strlen(names[i])) == 0) known = true;
        if (!known) continue;
        if (access((base + "/driver").c_str(), F_OK) == 0) ++bound;
        else ++unbound;
    }
    closedir(d);
    return !(bound == 0 && unbound > 0);
}

static double nowMs() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void setOutputs(int ok, int ng, int err) {
    if (g_p.pinErr < 0) {  // no separate error line: error = OK and NG both high
        g_ioOk.write(ok || err);
        g_ioNg.write(ng || err);
        return;
    }
    g_ioOk.write(ok);
    g_ioNg.write(ng);
    g_ioErr.write(err);
}

// ---------------------------------------------------------------------------------------------
// detection
// ---------------------------------------------------------------------------------------------

// Circles are searched only inside the detection area (ROI); everything outside it is ignored.
// The returned centres are in "work" pixels of the whole picture (camera pixels * 0.5), ROI offset included.
static int countCircles(const cv::Mat& gray, const Settings& s, std::vector<cv::Vec3f>& circles) {
    double k = (double)WORK_W / gray.cols;  // camera pixels -> work pixels
    int minR = std::max(2, cvRound(s.minDia * 0.5 * k));
    int maxR = std::max(minR + 1, cvRound(s.maxDia * 0.5 * k));
    cv::Rect roi(s.roiX, s.roiY, s.roiW, s.roiH);
    roi &= cv::Rect(0, 0, gray.cols, gray.rows);
    if (roi.width < 8 || roi.height < 8) roi = cv::Rect(0, 0, gray.cols, gray.rows);
    cv::Mat small;
    cv::resize(gray(roi), small, cv::Size(std::max(8, cvRound(roi.width * k)), std::max(8, cvRound(roi.height * k))),
               0, 0, cv::INTER_AREA);
    cv::GaussianBlur(small, small, cv::Size(5, 5), 1.5);
    circles.clear();
    cv::HoughCircles(small, circles, cv::HOUGH_GRADIENT, 1.0, std::max(8, cvRound(minR * 1.5)),
                     s.edge, s.roundness, minR, maxR);
    for (size_t i = 0; i < circles.size(); ++i) {
        circles[i][0] += (float)(roi.x * k);
        circles[i][1] += (float)(roi.y * k);
    }
    return (int)circles.size();
}

// Dark-hole mode: a hole is a patch that is darker than its surroundings, so no clear edge is needed.
//   1. cut out the detection area (a small area is searched at full size, a large one at half size)
//   2. subtract the local average brightness: holes are what stays well above "contrast" grey levels
//   3. keep the blobs whose size and roundness fit and that do not touch the border of the area
// Returned circles use the same convention as countCircles(): centre/radius in half-size camera pixels.
static int countDarkHoles(const cv::Mat& gray, const Settings& s, std::vector<cv::Vec3f>& circles) {
    cv::Rect roi(s.roiX, s.roiY, s.roiW, s.roiH);
    roi &= cv::Rect(0, 0, gray.cols, gray.rows);
    if (roi.width < 8 || roi.height < 8) roi = cv::Rect(0, 0, gray.cols, gray.rows);
    double k = std::min(1.0, std::min((double)WORK_W / roi.width, (double)WORK_H / roi.height));

    cv::Mat img;
    cv::resize(gray(roi), img, cv::Size(std::max(8, cvRound(roi.width * k)), std::max(8, cvRound(roi.height * k))),
               0, 0, cv::INTER_AREA);
    cv::GaussianBlur(img, img, cv::Size(3, 3), 0);

    // averaging window: wider than the biggest hole, but never so wide that it reaches other bright or dark
    // areas (a big dark plate must not count as "darker than its surroundings")
    int win = cvRound(s.maxDia * k * 2.0);
    win = std::min(61, std::max(15, win));
    if (win % 2 == 0) ++win;
    cv::Mat bg, diff, mask;
    cv::boxFilter(img, bg, -1, cv::Size(win, win), cv::Point(-1, -1), true, cv::BORDER_REPLICATE);
    cv::subtract(bg, img, diff);  // > 0 where the pixel is darker than its surroundings
    cv::threshold(diff, mask, s.contrast, 255, cv::THRESH_BINARY);
    cv::morphologyEx(mask, mask, cv::MORPH_OPEN, cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(3, 3)));

    std::vector<std::vector<cv::Point> > contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);
    circles.clear();
    const double minD = s.minDia * k, maxD = s.maxDia * k;
    for (size_t i = 0; i < contours.size(); ++i) {
        double area = cv::contourArea(contours[i]);
        if (area < 3) continue;
        double d = 2.0 * std::sqrt(area / CV_PI);  // diameter of the circle with the same area
        if (d < minD || d > maxD) continue;
        cv::Rect bb = cv::boundingRect(contours[i]);
        if (bb.x <= 0 || bb.y <= 0 || bb.x + bb.width >= mask.cols || bb.y + bb.height >= mask.rows) continue;  // cut off
        double per = cv::arcLength(contours[i], true);
        if (per <= 0) continue;
        double circ = std::min(1.0, 4.0 * CV_PI * area / (per * per));
        if (circ * 100.0 < s.shape) continue;
        cv::Moments m = cv::moments(contours[i]);
        if (m.m00 <= 0) continue;
        double cx = (m.m10 / m.m00) / k + roi.x, cy = (m.m01 / m.m00) / k + roi.y;  // camera pixels
        circles.push_back(cv::Vec3f((float)(cx * 0.5), (float)(cy * 0.5), (float)(d / k * 0.25)));
    }
    return (int)circles.size();
}

static int detect(const cv::Mat& gray, const Settings& s, std::vector<cv::Vec3f>& circles) {
    return s.mode == 1 ? countDarkHoles(gray, s, circles) : countCircles(gray, s, circles);
}

// Own ISP start-up (same steps as the Rockchip sample): the public sample library does not hand out
// the AIQ context that exposure control needs.
static bool ispStart(const char* iqDir) {
    rk_aiq_static_info_t info;
    memset(&info, 0, sizeof(info));
    if (rk_aiq_uapi2_sysctl_enumStaticMetas(0, &info) != XCAM_RETURN_NO_ERROR) return false;
    rk_aiq_uapi2_sysctl_preInit_devBufCnt(info.sensor_info.sensor_name, "rkraw_rx", 2);
    g_aiq = rk_aiq_uapi2_sysctl_init(info.sensor_info.sensor_name, iqDir, NULL, NULL);
    if (!g_aiq) return false;
    if (rk_aiq_uapi2_sysctl_prepare(g_aiq, 0, 0, RK_AIQ_WORKING_MODE_NORMAL) != XCAM_RETURN_NO_ERROR) return false;
    return rk_aiq_uapi2_sysctl_start(g_aiq) == XCAM_RETURN_NO_ERROR;
}

static void ispStop() {
    if (!g_aiq) return;
    rk_aiq_uapi2_sysctl_stop(g_aiq, false);
    rk_aiq_uapi2_sysctl_deinit(g_aiq);
    g_aiq = NULL;
}

// Fixed brightness: with a manual exposure time and/or sensor gain the picture no longer brightens up when the
// scene is dark (which is what makes dark areas such as the inside of the housing show up). 0 = automatic.
static bool applyExposure(int x10, int gain10) {
    if (!g_aiq) return false;
    Uapi_ExpSwAttrV2_t a{};
    XCamReturn r = rk_aiq_user_api2_ae_getExpSwAttr(g_aiq, &a);
    if (r != XCAM_RETURN_NO_ERROR && r != XCAM_RETURN_BYPASS) a.Enable = true;
    a.sync.sync_mode = RK_AIQ_UAPI_MODE_ASYNC;
    a.sync.done = false;
    if (x10 <= 0 && gain10 <= 0) {
        a.AecOpType = RK_AIQ_OP_MODE_AUTO;
    } else {
        a.AecOpType = RK_AIQ_OP_MODE_MANUAL;
        a.stManual.LinearAE.ManualTimeEn = x10 > 0;
        a.stManual.LinearAE.TimeValue = x10 / 10000.0f;  // seconds
        a.stManual.LinearAE.ManualGainEn = gain10 > 0;
        a.stManual.LinearAE.GainValue = gain10 / 10.0f;
        a.stManual.LinearAE.ManualIspDgainEn = false;
    }
    XCamReturn rs = rk_aiq_user_api2_ae_setExpSwAttr(g_aiq, a);
    printf("exposure %d (0.1ms) gain %d (0.1x): set=%d\n", x10, gain10, (int)rs);
    fflush(stdout);
    return rs == XCAM_RETURN_NO_ERROR;
}

// ---------------------------------------------------------------------------------------------
// system load, shown on the bench camera view
// ---------------------------------------------------------------------------------------------

struct SysStats {
    double cpuPct = 0, memUsedMb = 0, memTotalMb = 0, rssMb = 0, tempC = -1;
};

static void refreshSysStats(SysStats& s) {
    static double lastMs = 0;
    static unsigned long long lastBusy = 0, lastTotal = 0;
    double now = nowMs();
    if (now - lastMs < 500) return;
    lastMs = now;
    FILE* f = fopen("/proc/stat", "r");
    if (f) {
        unsigned long long u = 0, n = 0, sy = 0, id = 0, io = 0, irq = 0, sirq = 0, st = 0;
        if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &sy, &id, &io, &irq, &sirq, &st) >= 4) {
            unsigned long long total = u + n + sy + id + io + irq + sirq + st, busy = total - id - io;
            if (lastTotal && total > lastTotal) s.cpuPct = 100.0 * (busy - lastBusy) / (total - lastTotal);
            lastBusy = busy;
            lastTotal = total;
        }
        fclose(f);
    }
    f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[128];
        double total = 0, avail = 0;
        while (fgets(line, sizeof(line), f)) {
            sscanf(line, "MemTotal: %lf kB", &total);
            sscanf(line, "MemAvailable: %lf kB", &avail);
        }
        fclose(f);
        s.memTotalMb = total / 1024;
        s.memUsedMb = (total - avail) / 1024;
    }
    f = fopen("/proc/self/statm", "r");
    if (f) {
        unsigned long size = 0, rss = 0;
        if (fscanf(f, "%lu %lu", &size, &rss) == 2) s.rssMb = rss * 4.0 / 1024;
        fclose(f);
    }
    f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (f) {
        int t = 0;
        if (fscanf(f, "%d", &t) == 1) s.tempC = t / 1000.0;
        fclose(f);
    }
}

static void publishCamView(const cv::Mat& gray, const std::vector<cv::Vec3f>& circles, int count, int result,
                           double fps, double ms) {
    static SysStats sys;
    refreshSysStats(sys);
    cv::Mat vis;
    cv::cvtColor(gray, vis, cv::COLOR_GRAY2BGR);
    double k = (double)gray.cols / WORK_W;
    for (size_t i = 0; i < circles.size(); ++i)
        cv::circle(vis, cv::Point(cvRound(circles[i][0] * k), cvRound(circles[i][1] * k)),
                   cvRound(circles[i][2] * k), cv::Scalar(0, 255, 0), 2);
    char t[96];
    const char* r = result == 1 ? "OK" : (result == 0 ? "NG" : (result == 2 ? "ERR" : "--"));
    snprintf(t, sizeof(t), "circles=%d  %s", count, r);
    cv::putText(vis, t, cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.9, cv::Scalar(0, 0, 255), 2);
    char info[3][96];
    snprintf(info[0], sizeof(info[0]), "fps %.1f   detect %.1f ms", fps, ms);
    snprintf(info[1], sizeof(info[1]), "CPU %.0f%%  mem %.1f MB  bright %.0f", sys.cpuPct, sys.rssMb, cv::mean(gray)[0]);
    snprintf(info[2], sizeof(info[2]), "RAM %.0f/%.0f MB   %.0f C", sys.memUsedMb, sys.memTotalMb, sys.tempC);
    for (int i = 0; i < 3; ++i) {
        cv::putText(vis, info[i], cv::Point(10, 62 + i * 26), cv::FONT_HERSHEY_SIMPLEX, 0.62, cv::Scalar(0, 0, 0), 3);
        cv::putText(vis, info[i], cv::Point(10, 62 + i * 26), cv::FONT_HERSHEY_SIMPLEX, 0.62, cv::Scalar(0, 255, 255), 1);
    }
    webPublishCam(vis);
}

// ---------------------------------------------------------------------------------------------
// threads
// ---------------------------------------------------------------------------------------------

// Waits for the PLC trigger edge. The outputs are cleared right here so the PLC sees "busy"
// immediately, not after the next camera frame.
static void* triggerThread(void*) {
    while (g_run) {
        if (g_ioTrigger.waitEdge(100) && !g_menuOpen) {
            g_trigAt = nowMs();
            ++g_trigCount;
            setOutputs(0, 0, 0);
            g_trigPending = 1;
        }
    }
    return NULL;
}

// Draws the LCD picture, handles the keys and publishes the same picture to the bench page.
static void* uiThread(void*) {
    Ui ui(g_shared, g_p.conf);
    ui.setTouchDebug(g_p.touchDebug);
    ui.setTouchAvailable(!g_p.touchDev.empty());  // without a touch panel the hints talk about keys
    Display disp;
    bool haveLcd = false;
    if (!g_p.noScreen) {
        haveLcd = disp.open(g_p.spidev.c_str(), g_p.pinDc, g_p.pinRst, g_p.pinBl, g_p.spiHz);
        double up = 0;
        FILE* uf = fopen("/proc/uptime", "r");
        if (uf) {
            if (fscanf(uf, "%lf", &up) != 1) up = 0;
            fclose(uf);
        }
        printf("lcd: %s (%.1f s after power-on)\n", haveLcd ? "ready" : "not available (no spidev? bench page still works)", up);
    }
    bool backlightOn = false;
    cv::Mat frame;
    while (g_run) {
        double t0 = nowMs();
        Key k;
        while (keysPop(k)) ui.onKey(k);
        TouchEvent te;
        while (touchPop(te)) {
            if (g_p.touchDebug && te.down && !te.repeat) printf("touch x=%d y=%d\n", te.x, te.y);
            ui.onTouch(te);
        }
        g_menuOpen = ui.menuOpen();
        Snapshot snap;
        pthread_mutex_lock(&g_snapLock);
        snap = g_snap;
        pthread_mutex_unlock(&g_snapLock);
        if (snap.preview.empty()) snap.status = getStatus();
        ui.render(frame, snap);
        if (haveLcd) {
            disp.show(frame);
            if (!backlightOn) {  // light up only once the first picture is in the panel
                disp.backlight(true);
                backlightOn = true;
            }
        }
        if (webWantsUi()) webPublishUi(frame);
        double spent = nowMs() - t0;
        if (spent < 33) usleep((useconds_t)((33 - spent) * 1000));
    }
    if (haveLcd) disp.backlight(false);
    return NULL;
}

// ---------------------------------------------------------------------------------------------
// command line
// ---------------------------------------------------------------------------------------------

static void printUsage() {
    printf("usage: luckfox_jpeg_server [options]\n"
           "  --continuous            detect on every frame, ignore the PLC trigger (bench use)\n"
           "  --web PORT              bench page: camera, LCD picture and keys in a browser\n"
           "  --image FILE            run the detector on a picture and exit (no camera)\n"
           "  --pins T,OK,NG,ERR      trigger input and result outputs (GPIO numbers, ERR -1 = error is OK+NG both high)\n"
           "  --lcd DC,RST,BL         LCD control pins (BL -1 = backlight wired to 3.3V)\n"
           "  --spi DEV               LCD SPI device (default /dev/spidev0.0)\n"
           "  --touch DEV             touch controller I2C bus, e.g. /dev/i2c-3   --touch-rst N  its reset GPIO\n"
           "  --keys gpio|adc|none    key source                  --keypins OK,UP,DOWN,LEFT,RIGHT,ADJ\n"
           "  --adc N                 SARADC channel for the resistor-ladder keys\n"
           "  --no-screen             do not open the LCD\n"
           "  --conf FILE             saved settings (default /userdata/circle.conf)\n"
           "  --target N --dmin N --dmax N --round N --edge N --exposure N   override saved settings\n");
}

static bool parseArgs(int argc, char** argv, Params& p) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        const char* v = NULL;
        bool needs = (a != "--continuous" && a != "--no-screen" && a != "--help");
        if (needs) {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a.c_str()); return false; }
            v = argv[++i];
        }
        if (a == "--continuous") p.continuous = true;
        else if (a == "--no-screen") p.noScreen = true;
        else if (a == "--help") { printUsage(); exit(0); }
        else if (a == "--web" || a == "--stream") p.webPort = atoi(v);
        else if (a == "--image") p.imagePath = v;
        else if (a == "--conf") p.conf = v;
        else if (a == "--spi") p.spidev = v;
        else if (a == "--keys") p.keyMode = v;
        else if (a == "--adc") p.adcChannel = atoi(v);
        else if (a == "--touch") p.touchDev = v;
        else if (a == "--touch-rst") p.touchRst = atoi(v);
        else if (a == "--wait-file") p.waitFile = v;
        else if (a == "--auto-restart") p.autoRestart = atoi(v);
        else if (a == "--touch-debug") p.touchDebug = (atoi(v) != 0);
        else if (a == "--target") p.oTarget = atoi(v);
        else if (a == "--mode") p.oMode = atoi(v);
        else if (a == "--contrast") p.oContrast = atoi(v);
        else if (a == "--shape") p.oShape = atoi(v);
        else if (a == "--dmin") p.oMinDia = atoi(v);
        else if (a == "--dmax") p.oMaxDia = atoi(v);
        else if (a == "--round") p.oRound = atoi(v);
        else if (a == "--edge") p.oEdge = atoi(v);
        else if (a == "--exposure") p.oExposure = atoi(v);
        else if (a == "--pins") {
            if (sscanf(v, "%d,%d,%d,%d", &p.pinTrigger, &p.pinOk, &p.pinNg, &p.pinErr) != 4) return false;
        } else if (a == "--lcd") {
            if (sscanf(v, "%d,%d,%d", &p.pinDc, &p.pinRst, &p.pinBl) != 3) return false;
        } else if (a == "--keypins") {
            if (sscanf(v, "%d,%d,%d,%d,%d,%d", &p.keyPins[0], &p.keyPins[1], &p.keyPins[2], &p.keyPins[3],
                       &p.keyPins[4], &p.keyPins[5]) != 6) return false;
        } else {
            fprintf(stderr, "unknown argument: %s\n", a.c_str());
            printUsage();
            return false;
        }
    }
    return true;
}

static Settings initialSettings() {
    Settings s;
    s.load(g_p.conf.c_str());  // missing file is fine: defaults
    if (g_p.oTarget >= 0) s.target = g_p.oTarget;
    if (g_p.oMinDia >= 0) s.minDia = g_p.oMinDia;
    if (g_p.oMaxDia >= 0) s.maxDia = g_p.oMaxDia;
    if (g_p.oRound >= 0) s.roundness = g_p.oRound;
    if (g_p.oEdge >= 0) s.edge = g_p.oEdge;
    if (g_p.oExposure >= 0) s.exposure = g_p.oExposure;
    if (g_p.oMode >= 0) s.mode = g_p.oMode;
    if (g_p.oContrast >= 0) s.contrast = g_p.oContrast;
    if (g_p.oShape >= 0) s.shape = g_p.oShape;
    s.clamp();
    return s;
}

// ---------------------------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------------------------

// The camera cannot be used: tell the user on the LCD, raise the ERR output, and wait. Exiting would only make
// the autostart script restart us every few seconds.
static int holdError(const char* message, pthread_t uiTid) {
    setStatus(message);
    setOutputs(0, 0, 1);
    while (g_run) usleep(200000);
    if (uiTid) pthread_join(uiTid, NULL);
    keysStop();
    touchStop();
    webStop();
    setOutputs(0, 0, 0);
    return 0;
}

// ---- camera problem at boot: restart automatically a few times, keep a record of what the board looked like ----
// The kernel looks for the sensor once, at boot. If it is not answering then (ribbon cable contact, power ramp), the
// picture can only come back with a reboot, so the board restarts itself up to --auto-restart times. The counter and
// the record live in /userdata so they survive the restart; the counter is cleared as soon as the camera works.
static const char* kRetryFile = "/userdata/camera_retries";
static const char* kFailLog = "/userdata/camera_fail.log";

static std::string shellOut(const char* cmd) {
    std::string out;
    FILE* p = popen(cmd, "r");
    if (!p) return out;
    char buf[256];
    while (out.size() < 6000 && fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
    return out;
}

static int readRetryCount() {
    int n = 0;
    FILE* f = fopen(kRetryFile, "r");
    if (f) {
        if (fscanf(f, "%d", &n) != 1) n = 0;
        fclose(f);
    }
    return n;
}

static void writeRetryCount(int n) {
    FILE* f = fopen(kRetryFile, "w");
    if (f) {
        fprintf(f, "%d\n", n);
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
}

static double uptimeSeconds() {
    double up = 0;
    FILE* f = fopen("/proc/uptime", "r");
    if (f) {
        if (fscanf(f, "%lf", &up) != 1) up = 0;
        fclose(f);
    }
    return up;
}

static void appendFailLog(const std::string& text) {
    struct stat st;
    if (stat(kFailLog, &st) == 0 && st.st_size > 24000) unlink(kFailLog);  // keep the flash file small
    FILE* f = fopen(kFailLog, "a");
    if (!f) return;
    fputs(text.c_str(), f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}

static void recordCameraFailure(const char* why, int attempt) {
    char head[160];
    snprintf(head, sizeof(head), "=== camera problem: %s | %.0f s after power-on | automatic restarts so far: %d ===\n",
             why, uptimeSeconds(), attempt);
    std::string rec = head;
    rec += "-- sensor probe result at boot (the key lines)\n";
    rec += shellOut("dmesg | grep -iE 'Unexpected sensor id|Detected|No link|reset-gpios' | head -n 14");
    rec += "-- kernel messages about the camera\n";
    rec += shellOut("dmesg | grep -iE 'sc3336|sc4336|sc530ai|dphy|rkcif|csi2|sensor' | tail -n 20");
    rec += "-- I2C buses and a scan of the camera bus\n";
    rec += shellOut("i2cdetect -l 2>&1 | head -8; i2cdetect -y 4 2>&1 | head -10");
    rec += "-- sensor devices and their drivers\n";
    rec += shellOut("for d in /sys/bus/i2c/devices/4-*; do echo \"$(basename $d) $(cat $d/name 2>/dev/null) driver=$(basename $(readlink $d/driver 2>/dev/null) 2>/dev/null)\"; done");
    rec += "-- USB / power related kernel messages, load, temperature\n";
    rec += shellOut("dmesg | grep -iE 'usb|dwc3|power|volt' | tail -n 6; cat /proc/loadavg; "
                    "echo \"temp $(cat /sys/class/thermal/thermal_zone0/temp)\"");
    rec += "\n";
    appendFailLog(rec);
}

// Called when the camera did not come up. Returns main()'s exit code.
static int cameraProblem(const char* why, bool notFound, pthread_t uiTid) {
    int tries = readRetryCount();
    recordCameraFailure(why, tries);
    if (g_p.autoRestart > 0 && tries < g_p.autoRestart) {
        writeRetryCount(tries + 1);
        char msg[120];
        snprintf(msg, sizeof(msg), "CAMERA %s\nrestarting automatically\n(%d of %d)", notFound ? "NOT FOUND" : "FAILED",
                 tries + 1, g_p.autoRestart);
        setStatus(msg);
        setOutputs(0, 0, 1);
        for (int i = 0; i < 50 && g_run; ++i) usleep(100000);  // five seconds, long enough to read
        sync();
        if (system("reboot") != 0) fprintf(stderr, "reboot command failed\n");
        while (g_run) usleep(200000);  // the reboot takes over
        return 0;
    }
    if (notFound) return holdError("CAMERA NOT FOUND\ncheck the ribbon cable,\nthen power-cycle", uiTid);
    return holdError("CAMERA INIT FAILED\npower-cycle the board", uiTid);
}

// The camera works: forget earlier failures and note that the restart helped.
static void cameraOk() {
    int n = readRetryCount();
    if (access(kRetryFile, F_OK) != 0) return;
    unlink(kRetryFile);
    if (n > 0) {
        char line[120];
        snprintf(line, sizeof(line), "=== camera recovered after %d automatic restart(s), %.0f s after power-on ===\n\n", n,
                 uptimeSeconds());
        appendFailLog(line);
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!parseArgs(argc, argv, g_p)) return 1;
    Settings settings = initialSettings();

    if (!g_p.imagePath.empty()) {  // offline check of the detector
        cv::Mat img = cv::imread(g_p.imagePath, cv::IMREAD_GRAYSCALE);
        if (img.empty()) { fprintf(stderr, "cannot read %s\n", g_p.imagePath.c_str()); return 1; }
        std::vector<cv::Vec3f> circles;
        int count = 0;
        double t0 = nowMs();
        for (int i = 0; i < 20; ++i) count = detect(img, settings, circles);
        printf("%s: circles=%d -> %s  (%.1f ms/run)\n", g_p.imagePath.c_str(), count,
               count == settings.target ? "OK" : "NG", (nowMs() - t0) / 20);
        return 0;
    }

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    g_shared.set(settings);

    // The screen comes first: logo and a progress message as early as possible, then everything else is set up
    // (and the camera, if it does not come up, is explained on the screen).
    pthread_t trigTid = 0, uiTid = 0;
    setStatus("Starting system");
    pthread_create(&uiTid, NULL, uiThread, NULL);

    bool ioOk = g_ioOk.openOut(g_p.pinOk, 0) && g_ioNg.openOut(g_p.pinNg, 0) && (g_p.pinErr < 0 || g_ioErr.openOut(g_p.pinErr, 0));
    if (!g_p.continuous) ioOk = ioOk && g_ioTrigger.openIn(g_p.pinTrigger, "rising");
    if (!ioOk) fprintf(stderr, "WARNING: GPIO setup failed (check pin numbers)\n");

    bool keysOk = false;
    if (g_p.keyMode == "gpio") keysOk = keysStartGpio(g_p.keyPins);
    else if (g_p.keyMode == "adc") keysOk = keysStartAdc(g_p.adcChannel);
    else if (g_p.keyMode == "mixed") keysOk = keysStartMixed(g_p.keyPins, g_p.adcChannel);
    else if (g_p.keyMode == "pad") keysOk = keysStartMixed(g_p.keyPins, g_p.adcChannel, true);
    printf("keys: %s (%s)\n", g_p.keyMode.c_str(), keysOk ? "ready" : "none");

    bool touchOk = !g_p.touchDev.empty() && touchStart(g_p.touchDev.c_str(), g_p.touchRst);
    printf("touch: %s\n", g_p.touchDev.empty() ? "off" : (touchOk ? "ready" : "not answering"));

    if (g_p.webPort > 0) webStart(g_p.webPort);

    if (!sensorPresent()) {
        fprintf(stderr, "no camera sensor was detected at boot\n");
        return cameraProblem("sensor not detected at boot", true, uiTid);
    }
    // The stock camera app holds the camera until the autostart script has stopped it and creates this file.
    // Waiting here (after the screen is up) keeps the screen from staying dark during that time.
    if (!g_p.waitFile.empty()) {
        setStatus("Preparing camera");
        for (int i = 0; i < 1500 && g_run && access(g_p.waitFile.c_str(), F_OK) != 0; ++i) usleep(100000);
    }
    setStatus("Starting camera");
    if (!ispStart("/etc/iqfiles")) {
        fprintf(stderr, "ISP init failed\n");
        return cameraProblem("ISP init failed", false, uiTid);
    }
    if (RK_MPI_SYS_Init() != RK_SUCCESS || vi_dev_init() != 0 || vi_chn_init(0, CAP_WIDTH, CAP_HEIGHT) != 0) {
        fprintf(stderr, "MPI/VI init failed\n");
        ispStop();
        return cameraProblem("MPI/VI init failed", false, uiTid);
    }
    cameraOk();  // the camera is up: clear the restart counter
    // Automatic white balance only makes the grey levels drift; this is a brightness-only job.
    if (g_aiq) rk_aiq_uapi2_sysctl_setModuleCtl(g_aiq, RK_MODULE_AWB, false);
    // exposure and gain are applied from the capture loop once frames are flowing

    setStatus("");
    if (!g_p.continuous) pthread_create(&trigTid, NULL, triggerThread, NULL);
    printf("ready: %s mode\n", g_p.continuous ? "continuous" : "PLC trigger");

    int appliedExposure = -1, appliedGain = -1;  // forces the first apply after the first frame
    int trig = 0;  // 0 idle, 2 = next frame is the one to measure
    int count = 0, result = -1;
    double detectMs = 0, fps = 0, fpsStart = nowMs();
    int fpsFrames = 0;
    std::vector<cv::Vec3f> circles;
    Snapshot frozen;
    bool haveFrozen = false;

    while (g_run) {
        VIDEO_FRAME_INFO_S frame;
        if (RK_MPI_VI_GetChnFrame(0, 0, &frame, 500) != RK_SUCCESS) {
            if (trig == 2 || g_trigPending) {
                result = 2;
                haveFrozen = false;  // show the error, not the last good measurement
                setOutputs(0, 0, 1);
                trig = 0;
                g_trigPending = 0;
            }
            continue;
        }
        void* base = RK_MPI_MB_Handle2VirAddr(frame.stVFrame.pMbBlk);
        cv::Mat gray(CAP_HEIGHT, CAP_WIDTH, CV_8UC1, base, frame.stVFrame.u32VirWidth);

        Settings s = g_shared.get();
        if (s.exposure != appliedExposure || s.gain != appliedGain) {
            applyExposure(s.exposure, s.gain);
            appliedExposure = s.exposure;
            appliedGain = s.gain;
        }

        // A trigger seen now may have arrived while this frame was exposing: skip it, measure the next.
        bool measure = false;
        if (g_trigPending) {
            g_trigPending = 0;
            trig = 2;
        } else if (trig == 2) {
            measure = true;
            trig = 0;
        }

        if (measure || g_menuOpen || g_p.continuous) {
            double t0 = nowMs();
            count = detect(gray, s, circles);
            detectMs = nowMs() - t0;
            if (measure || g_p.continuous) {
                result = (count == s.target) ? 1 : 0;
                setOutputs(result == 1, result == 0, 0);
                static int lastPrinted = -2;
                static double lastPrintMs = 0;
                if (result != lastPrinted || !g_p.continuous || nowMs() - lastPrintMs > 2000) {  // the log lives in RAM
                    printf("circles=%d -> %s (%.1f ms) brightness=%.0f\n", count, result == 1 ? "OK" : "NG",
                           detectMs, cv::mean(gray)[0]);
                    lastPrinted = result;
                    lastPrintMs = nowMs();
                }
            }
        }

        ++fpsFrames;
        if (nowMs() - fpsStart >= 1000) {
            fps = fpsFrames * 1000.0 / (nowMs() - fpsStart);
            fpsFrames = 0;
            fpsStart = nowMs();
        }

        // picture for the LCD thread
        Snapshot snap;
        cv::Mat small;
        cv::resize(gray, small, cv::Size(379, 284), 0, 0, cv::INTER_AREA);  // full screen height, 4:3 stays 4:3
        cv::cvtColor(small, snap.preview, cv::COLOR_GRAY2BGR);
        double ks = 284.0 / CAP_HEIGHT * (CAP_WIDTH / (double)WORK_W);  // work pixels -> preview pixels
        for (size_t i = 0; i < circles.size(); ++i)
            snap.circles.push_back(cv::Vec3f(circles[i][0] * ks, circles[i][1] * ks, circles[i][2] * ks));
        snap.count = count;
        snap.result = result;
        snap.fps = fps;
        snap.detectMs = detectMs;
        // After a PLC trigger the screen keeps the measured picture until the next trigger; the menu shows live video.
        snap.trigNo = g_trigCount;
        if (measure && !g_p.continuous) {
            snap.totalMs = nowMs() - g_trigAt;
            frozen = snap;
            frozen.held = true;
            haveFrozen = true;
        }
        pthread_mutex_lock(&g_snapLock);
        if (!g_p.continuous && !measure && (trig == 2 || g_trigPending)) {  // trigger seen, result still to come
            Snapshot waiting = haveFrozen ? frozen : snap;
            waiting.busy = true;
            waiting.trigNo = g_trigCount;
            g_snap = g_menuOpen ? snap : waiting;
        } else {
            g_snap = (haveFrozen && !g_menuOpen && !g_p.continuous) ? frozen : snap;
        }
        pthread_mutex_unlock(&g_snapLock);

        if (webWantsCam()) publishCamView(gray, circles, count, result, fps, detectMs);
        RK_MPI_VI_ReleaseChnFrame(0, 0, &frame);
    }

    g_run = false;
    if (trigTid) pthread_join(trigTid, NULL);
    if (uiTid) pthread_join(uiTid, NULL);
    keysStop();
    touchStop();
    webStop();
    setOutputs(0, 0, 0);
    RK_MPI_VI_DisableChn(0, 0);
    RK_MPI_VI_DisableDev(0);
    RK_MPI_SYS_Exit();
    ispStop();
    return 0;
}
