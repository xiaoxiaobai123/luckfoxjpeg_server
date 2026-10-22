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
    // GPIO numbers = bank * 32 + group * 8 + index (A=0..D=3). Defaults match the wiring plan for
    // both the Pico Max (RV1106) and the Pico Mini (RV1103).
    int pinTrigger = 55, pinOk = 56, pinNg = 57, pinErr = 58;
    int pinDc = 52, pinRst = 53, pinBl = 54;
    int keyPins[6] = {64, 65, 66, 67, 70, 69};  // OK, UP, DOWN, LEFT, RIGHT, ADJ
    std::string keyMode = "gpio";               // gpio | adc | none
    int adcChannel = 0;
    std::string spidev = "/dev/spidev0.0";
    int spiHz = 40000000;
    bool noScreen = false;
    int webPort = 0;
    bool continuous = false;
    std::string conf = "/userdata/circle.conf";
    std::string imagePath;
    // command-line overrides of the saved settings (-1 = not given)
    int oTarget = -1, oMinDia = -1, oMaxDia = -1, oRound = -1, oEdge = -1, oExposure = -1;
};

static Params g_p;
static SharedSettings g_shared;
static volatile bool g_run = true;
static volatile bool g_menuOpen = false;
static volatile int g_trigPending = 0;

static Gpio g_ioTrigger, g_ioOk, g_ioNg, g_ioErr;

static Snapshot g_snap;
static pthread_mutex_t g_snapLock = PTHREAD_MUTEX_INITIALIZER;
static rk_aiq_sys_ctx_t* g_aiq = NULL;

static void onSignal(int) { g_run = false; }

static double nowMs() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void setOutputs(int ok, int ng, int err) {
    g_ioOk.write(ok);
    g_ioNg.write(ng);
    g_ioErr.write(err);
}

// ---------------------------------------------------------------------------------------------
// detection
// ---------------------------------------------------------------------------------------------

static int countCircles(const cv::Mat& gray, const Settings& s, std::vector<cv::Vec3f>& circles) {
    double k = (double)WORK_W / gray.cols;  // camera pixels -> work pixels
    int minR = std::max(2, cvRound(s.minDia * 0.5 * k));
    int maxR = std::max(minR + 1, cvRound(s.maxDia * 0.5 * k));
    cv::Mat small;
    cv::resize(gray, small, cv::Size(WORK_W, WORK_H), 0, 0, cv::INTER_AREA);
    cv::GaussianBlur(small, small, cv::Size(5, 5), 1.5);
    circles.clear();
    cv::HoughCircles(small, circles, cv::HOUGH_GRADIENT, 1.0, std::max(8, cvRound(minR * 1.5)),
                     s.edge, s.roundness, minR, maxR);
    return (int)circles.size();
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

static bool applyExposure(int x10) {
    if (!g_aiq) return false;
    Uapi_ExpSwAttrV2_t a{};
    XCamReturn r = rk_aiq_user_api2_ae_getExpSwAttr(g_aiq, &a);
    if (r != XCAM_RETURN_NO_ERROR && r != XCAM_RETURN_BYPASS) a.Enable = true;
    a.sync.sync_mode = RK_AIQ_UAPI_MODE_ASYNC;
    a.sync.done = false;
    if (x10 <= 0) {
        a.AecOpType = RK_AIQ_OP_MODE_AUTO;
    } else {
        a.AecOpType = RK_AIQ_OP_MODE_MANUAL;
        a.stManual.LinearAE.ManualTimeEn = true;
        a.stManual.LinearAE.TimeValue = x10 / 10000.0f;  // seconds
        a.stManual.LinearAE.ManualGainEn = false;
        a.stManual.LinearAE.ManualIspDgainEn = false;
    }
    XCamReturn rs = rk_aiq_user_api2_ae_setExpSwAttr(g_aiq, a);
    Uapi_ExpSwAttrV2_t back{};
    XCamReturn rb = rk_aiq_user_api2_ae_getExpSwAttr(g_aiq, &back);
    printf("exposure %d (0.1ms): get=%d set=%d readback: op=%d time_en=%d time=%.5f s\n", x10, (int)r, (int)rs, (int)rb,
           (int)back.AecOpType, (int)back.stManual.LinearAE.ManualTimeEn, (double)back.stManual.LinearAE.TimeValue);
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
            setOutputs(0, 0, 0);
            g_trigPending = 1;
        }
    }
    return NULL;
}

// Draws the LCD picture, handles the keys and publishes the same picture to the bench page.
static void* uiThread(void*) {
    Ui ui(g_shared, g_p.conf);
    Display disp;
    bool haveLcd = false;
    if (!g_p.noScreen) {
        haveLcd = disp.open(g_p.spidev.c_str(), g_p.pinDc, g_p.pinRst, g_p.pinBl, g_p.spiHz);
        printf("lcd: %s\n", haveLcd ? "ready" : "not available (no spidev? bench page still works)");
    }
    cv::Mat frame;
    while (g_run) {
        double t0 = nowMs();
        Key k;
        while (keysPop(k)) ui.onKey(k);
        g_menuOpen = ui.menuOpen();
        Snapshot snap;
        pthread_mutex_lock(&g_snapLock);
        snap = g_snap;
        pthread_mutex_unlock(&g_snapLock);
        ui.render(frame, snap);
        if (haveLcd) disp.show(frame);
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
           "  --pins T,OK,NG,ERR      trigger input and result outputs (GPIO numbers)\n"
           "  --lcd DC,RST,BL         LCD control pins            --spi DEV   (default /dev/spidev0.0)\n"
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
        else if (a == "--target") p.oTarget = atoi(v);
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
    s.clamp();
    return s;
}

// ---------------------------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------------------------

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
        for (int i = 0; i < 20; ++i) count = countCircles(img, settings, circles);
        printf("%s: circles=%d -> %s  (%.1f ms/run)\n", g_p.imagePath.c_str(), count,
               count == settings.target ? "OK" : "NG", (nowMs() - t0) / 20);
        return 0;
    }

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    g_shared.set(settings);

    bool ioOk = g_ioOk.openOut(g_p.pinOk, 0) && g_ioNg.openOut(g_p.pinNg, 0) && g_ioErr.openOut(g_p.pinErr, 0);
    if (!g_p.continuous) ioOk = ioOk && g_ioTrigger.openIn(g_p.pinTrigger, "rising");
    if (!ioOk) fprintf(stderr, "WARNING: GPIO setup failed (check pin numbers)\n");

    bool keysOk = false;
    if (g_p.keyMode == "gpio") keysOk = keysStartGpio(g_p.keyPins);
    else if (g_p.keyMode == "adc") keysOk = keysStartAdc(g_p.adcChannel);
    printf("keys: %s (%s)\n", g_p.keyMode.c_str(), keysOk ? "ready" : "none");

    if (!ispStart("/etc/iqfiles")) {
        fprintf(stderr, "ISP init failed\n");
        setOutputs(0, 0, 1);
        return 1;
    }
    if (RK_MPI_SYS_Init() != RK_SUCCESS || vi_dev_init() != 0 || vi_chn_init(0, CAP_WIDTH, CAP_HEIGHT) != 0) {
        fprintf(stderr, "MPI/VI init failed\n");
        setOutputs(0, 0, 1);
        ispStop();
        return 1;
    }
    // exposure is applied from the capture loop once frames are flowing

    if (g_p.webPort > 0) webStart(g_p.webPort);
    pthread_t trigTid = 0, uiTid = 0;
    if (!g_p.continuous) pthread_create(&trigTid, NULL, triggerThread, NULL);
    pthread_create(&uiTid, NULL, uiThread, NULL);
    printf("ready: %s mode\n", g_p.continuous ? "continuous" : "PLC trigger");

    int appliedExposure = -1;  // forces the first apply after the first frame
    int trig = 0;  // 0 idle, 2 = next frame is the one to measure
    int count = 0, result = -1;
    double detectMs = 0, fps = 0, fpsStart = nowMs();
    int fpsFrames = 0;
    std::vector<cv::Vec3f> circles;

    while (g_run) {
        VIDEO_FRAME_INFO_S frame;
        if (RK_MPI_VI_GetChnFrame(0, 0, &frame, 500) != RK_SUCCESS) {
            if (trig == 2 || g_trigPending) {
                result = 2;
                setOutputs(0, 0, 1);
                trig = 0;
                g_trigPending = 0;
            }
            continue;
        }
        void* base = RK_MPI_MB_Handle2VirAddr(frame.stVFrame.pMbBlk);
        cv::Mat gray(CAP_HEIGHT, CAP_WIDTH, CV_8UC1, base, frame.stVFrame.u32VirWidth);

        Settings s = g_shared.get();
        if (s.exposure != appliedExposure) {
            applyExposure(s.exposure);
            appliedExposure = s.exposure;
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
            count = countCircles(gray, s, circles);
            detectMs = nowMs() - t0;
            if (measure || g_p.continuous) {
                result = (count == s.target) ? 1 : 0;
                setOutputs(result == 1, result == 0, 0);
                static int lastPrinted = -2;
                static double lastPrintMs = 0;
                if (result != lastPrinted || !g_p.continuous || nowMs() - lastPrintMs > 2000) {  // the log lives in RAM
                    printf("circles=%d -> %s (%.1f ms)\n", count, result == 1 ? "OK" : "NG", detectMs);
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
        cv::resize(gray, small, cv::Size(240, 180), 0, 0, cv::INTER_AREA);
        cv::cvtColor(small, snap.preview, cv::COLOR_GRAY2BGR);
        double ks = 240.0 / WORK_W;
        for (size_t i = 0; i < circles.size(); ++i)
            snap.circles.push_back(cv::Vec3f(circles[i][0] * ks, circles[i][1] * ks, circles[i][2] * ks));
        snap.count = count;
        snap.result = result;
        snap.fps = fps;
        snap.detectMs = detectMs;
        pthread_mutex_lock(&g_snapLock);
        g_snap = snap;
        pthread_mutex_unlock(&g_snapLock);

        if (webWantsCam()) publishCamView(gray, circles, count, result, fps, detectMs);
        RK_MPI_VI_ReleaseChnFrame(0, 0, &frame);
    }

    g_run = false;
    if (trigTid) pthread_join(trigTid, NULL);
    if (uiTid) pthread_join(uiTid, NULL);
    keysStop();
    webStop();
    setOutputs(0, 0, 0);
    RK_MPI_VI_DisableChn(0, 0);
    RK_MPI_VI_DisableDev(0);
    RK_MPI_SYS_Exit();
    ispStop();
    return 0;
}
