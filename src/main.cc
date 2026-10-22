// Luckfox Pico Pro/Max: camera -> circle detection -> GPIO result.
//
//   IO_TRIGGER (in)  : PLC rising edge = take a picture and judge
//   IO_RESULT  (out) : 1 = exactly 5 circles found (front), 0 = otherwise (back)
//   IO_DONE    (out) : 1 = result is valid; cleared when the trigger drops
//   IO_ERROR   (out) : 1 = camera or detection failure
//
// Usage:  luckfox_jpeg_server [--continuous] [--dump DIR] [--pins T,R,D,E] [--work WxH] [--canny N]
//                             [--rmin N] [--rmax N] [--p2 N] [--target N]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <string>
#include <vector>

extern "C" {
#include "rk_mpi_sys.h"
#include "rk_mpi_vi.h"
#include "rk_aiq_user_api2_sysctl.h"
}
#include "luckfox_mpi.h"
#include "gpio.h"
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#define CAP_WIDTH  640
#define CAP_HEIGHT 480

struct Params {
    int pinTrigger = 52;  // pin 7, GPIO1_C4
    int pinResult  = 55;  // pin 4, GPIO1_C7
    int pinDone    = 54;  // pin 5, GPIO1_C6
    int pinError   = 53;  // pin 6, GPIO1_C5
    int targetCount = 5;
    int workW = 320;      // detection runs on a downscaled gray image
    int workH = 240;
    int minRadius = 8;    // in work-image pixels, tune for the real part
    int maxRadius = 60;
    double cannyHigh = 100;
    double accum = 25;    // HoughCircles param2: lower = more circles
    bool continuous = false;
    std::string dumpDir;
    std::string imagePath;  // offline test: run detection on a file, no camera
};

static volatile bool g_run = true;

static void onSignal(int) { g_run = false; }

static double nowMs() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static int countCircles(const cv::Mat& gray, const Params& p, std::vector<cv::Vec3f>& circles) {
    cv::Mat small;
    cv::resize(gray, small, cv::Size(p.workW, p.workH), 0, 0, cv::INTER_AREA);
    cv::GaussianBlur(small, small, cv::Size(5, 5), 1.5);
    circles.clear();
    cv::HoughCircles(small, circles, cv::HOUGH_GRADIENT, 1.0, p.minRadius * 1.5,
                     p.cannyHigh, p.accum, p.minRadius, p.maxRadius);
    return (int)circles.size();
}

static void dumpFrame(const Params& p, const cv::Mat& gray, const std::vector<cv::Vec3f>& circles,
                      int count, int seq) {
    if (p.dumpDir.empty()) return;
    cv::Mat vis;
    cv::resize(gray, vis, cv::Size(p.workW, p.workH), 0, 0, cv::INTER_AREA);
    cv::cvtColor(vis, vis, cv::COLOR_GRAY2BGR);
    for (size_t i = 0; i < circles.size(); ++i)
        cv::circle(vis, cv::Point(cvRound(circles[i][0]), cvRound(circles[i][1])),
                   cvRound(circles[i][2]), cv::Scalar(0, 255, 0), 2);
    char path[256];
    snprintf(path, sizeof(path), "%s/frame_%04d_n%d.jpg", p.dumpDir.c_str(), seq, count);
    cv::imwrite(path, vis);
    snprintf(path, sizeof(path), "%s/raw_%04d.jpg", p.dumpDir.c_str(), seq);
    cv::imwrite(path, gray);
}

static bool parseArgs(int argc, char** argv, Params& p) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", name); return NULL; }
            return argv[++i];
        };
        const char* v = NULL;
        if (a == "--continuous") p.continuous = true;
        else if (a == "--dump")   { if (!(v = next("--dump"))) return false; p.dumpDir = v; }
        else if (a == "--image")  { if (!(v = next("--image"))) return false; p.imagePath = v; }
        else if (a == "--rmin")   { if (!(v = next("--rmin"))) return false; p.minRadius = atoi(v); }
        else if (a == "--rmax")   { if (!(v = next("--rmax"))) return false; p.maxRadius = atoi(v); }
        else if (a == "--p2")     { if (!(v = next("--p2"))) return false; p.accum = atof(v); }
        else if (a == "--canny")  { if (!(v = next("--canny"))) return false; p.cannyHigh = atof(v); }
        else if (a == "--work")   { if (!(v = next("--work"))) return false; if (sscanf(v, "%dx%d", &p.workW, &p.workH) != 2) return false; }
        else if (a == "--target") { if (!(v = next("--target"))) return false; p.targetCount = atoi(v); }
        else if (a == "--pins") {
            if (!(v = next("--pins"))) return false;
            if (sscanf(v, "%d,%d,%d,%d", &p.pinTrigger, &p.pinResult, &p.pinDone, &p.pinError) != 4) {
                fprintf(stderr, "--pins expects trigger,result,done,error\n");
                return false;
            }
        } else {
            fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

int main(int argc, char** argv) {
    Params p;
    if (!parseArgs(argc, argv, p)) return 1;
    if (!p.dumpDir.empty()) mkdir(p.dumpDir.c_str(), 0755);

    if (!p.imagePath.empty()) {
        cv::Mat img = cv::imread(p.imagePath, cv::IMREAD_GRAYSCALE);
        if (img.empty()) { fprintf(stderr, "cannot read %s\n", p.imagePath.c_str()); return 1; }
        std::vector<cv::Vec3f> circles;
        int count = 0;
        const int runs = 20;
        double t0 = nowMs();
        for (int i = 0; i < runs; ++i) count = countCircles(img, p, circles);
        printf("%s: circles=%d -> %s  (%.1f ms/run)\n", p.imagePath.c_str(), count,
               count == p.targetCount ? "FRONT(1)" : "BACK(0)", (nowMs() - t0) / runs);
        dumpFrame(p, img, circles, count, 0);
        return 0;
    }

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    Gpio ioTrigger, ioResult, ioDone, ioError;
    bool ioOk = ioResult.openOut(p.pinResult, 0) && ioDone.openOut(p.pinDone, 0) &&
                ioError.openOut(p.pinError, 0);
    if (!p.continuous) ioOk = ioOk && ioTrigger.openIn(p.pinTrigger, "rising");
    if (!ioOk) fprintf(stderr, "WARNING: GPIO setup failed (check pin numbers)\n");

    const RK_CHAR* iqDir = "/etc/iqfiles";
    RK_S32 ret = SAMPLE_COMM_ISP_Init(0, RK_AIQ_WORKING_MODE_NORMAL, RK_FALSE, iqDir);
    if (ret == RK_SUCCESS) ret = SAMPLE_COMM_ISP_Run(0);
    if (ret != RK_SUCCESS) {
        fprintf(stderr, "ISP init failed: %d\n", ret);
        ioError.write(1);
        return 1;
    }
    if (RK_MPI_SYS_Init() != RK_SUCCESS || vi_dev_init() != 0 ||
        vi_chn_init(0, CAP_WIDTH, CAP_HEIGHT) != 0) {
        fprintf(stderr, "MPI/VI init failed\n");
        ioError.write(1);
        SAMPLE_COMM_ISP_Stop(0);
        return 1;
    }
    printf("ready: %s mode, target=%d circles, radius %d-%d, p2=%.0f\n",
           p.continuous ? "continuous" : "PLC trigger", p.targetCount,
           p.minRadius, p.maxRadius, p.accum);

    int seq = 0;
    double statStart = nowMs();
    int statFrames = 0;
    while (g_run) {
        if (!p.continuous) {
            if (!ioTrigger.waitEdge(200)) continue;
            ioDone.write(0);
            ioError.write(0);
        }

        VIDEO_FRAME_INFO_S frame;
        // Drop one frame so the picture is exposed after the trigger, not before it.
        if (!p.continuous && RK_MPI_VI_GetChnFrame(0, 0, &frame, 200) == RK_SUCCESS)
            RK_MPI_VI_ReleaseChnFrame(0, 0, &frame);
        if (RK_MPI_VI_GetChnFrame(0, 0, &frame, 500) != RK_SUCCESS) {
            fprintf(stderr, "get frame failed\n");
            ioError.write(1);
            ioDone.write(1);
            if (!p.continuous) { while (g_run && ioTrigger.read() == 1) usleep(1000); ioDone.write(0); }
            continue;
        }

        // The Y plane of NV12 is already a gray image; no color conversion needed.
        void* base = RK_MPI_MB_Handle2VirAddr(frame.stVFrame.pMbBlk);
        cv::Mat gray(CAP_HEIGHT, CAP_WIDTH, CV_8UC1, base, frame.stVFrame.u32VirWidth);
        double t0 = nowMs();  // processing time only, excludes waiting for the frame

        std::vector<cv::Vec3f> circles;
        int count = countCircles(gray, p, circles);
        int front = (count == p.targetCount) ? 1 : 0;
        ioResult.write(front);
        ioDone.write(1);
        double t1 = nowMs();

        printf("#%d circles=%d -> %s  (%.1f ms)\n", seq, count, front ? "FRONT(1)" : "BACK(0)", t1 - t0);
        if (!p.dumpDir.empty() && (!p.continuous || seq % 30 == 0)) dumpFrame(p, gray, circles, count, seq);
        RK_MPI_VI_ReleaseChnFrame(0, 0, &frame);
        ++seq;

        if (p.continuous) {
            if (++statFrames == 30) {
                printf("avg %.1f fps\n", statFrames * 1000.0 / (nowMs() - statStart));
                statFrames = 0;
                statStart = nowMs();
            }
        } else {
            // Keep DONE high until the PLC drops the trigger, then reset for the next cycle.
            while (g_run && ioTrigger.read() == 1) usleep(1000);
            ioDone.write(0);
        }
    }

    ioDone.write(0);
    RK_MPI_VI_DisableChn(0, 0);
    RK_MPI_VI_DisableDev(0);
    RK_MPI_SYS_Exit();
    SAMPLE_COMM_ISP_Stop(0);
    return 0;
}
