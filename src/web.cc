#include "web.h"
#include "keys.h"

#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>

namespace {

struct Stream {
    pthread_mutex_t lock;
    std::vector<unsigned char> jpg;
    unsigned seq;
    volatile int clients;
    Stream() : seq(0), clients(0) { pthread_mutex_init(&lock, NULL); }
};

Stream g_cam, g_ui;
volatile bool g_run = false;
int g_listen = -1;
pthread_t g_serverThread;

const char* kPage =
    "<!doctype html><meta charset=utf-8><title>Circle checker</title>"
    "<style>body{font-family:sans-serif;background:#111;color:#ddd;margin:12px}"
    "img{background:#000;vertical-align:top}button{font-size:18px;margin:3px;padding:8px 14px}</style>"
    "<h3>Circle checker bench view</h3>"
    "<div><img src=/cam width=480> <img src=/ui width=240></div>"
    "<p>Screen keys: "
    "<button onclick=\"k('ok')\">OK / menu</button><button onclick=\"k('up')\">UP</button>"
    "<button onclick=\"k('down')\">DOWN</button><button onclick=\"k('left')\">LEFT</button>"
    "<button onclick=\"k('right')\">RIGHT</button><button onclick=\"k('adj')\">ADJ x10</button></p>"
    "<script>function k(n){fetch('/key?k='+n)}</script>";

void publish(Stream& s, const cv::Mat& bgr, int quality) {
    std::vector<unsigned char> jpg;
    std::vector<int> prm;
    prm.push_back(cv::IMWRITE_JPEG_QUALITY);
    prm.push_back(quality);
    cv::imencode(".jpg", bgr, jpg, prm);
    pthread_mutex_lock(&s.lock);
    s.jpg.swap(jpg);
    ++s.seq;
    pthread_mutex_unlock(&s.lock);
}

void sendAll(int fd, const std::string& s) { (void)send(fd, s.data(), s.size(), MSG_NOSIGNAL); }

void streamTo(int fd, Stream& s) {
    sendAll(fd, "HTTP/1.0 200 OK\r\nCache-Control: no-cache\r\n"
                "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n");
    __sync_fetch_and_add(&s.clients, 1);
    unsigned last = 0;
    bool ok = true;
    while (ok && g_run) {
        std::vector<unsigned char> copy;
        pthread_mutex_lock(&s.lock);
        unsigned seq = s.seq;
        if (seq != last) copy = s.jpg;
        pthread_mutex_unlock(&s.lock);
        if (seq == last || copy.empty()) {
            usleep(5000);
            continue;
        }
        last = seq;
        char part[128];
        int n = snprintf(part, sizeof(part),
                         "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", copy.size());
        ok = send(fd, part, n, MSG_NOSIGNAL) > 0 &&
             send(fd, (const char*)&copy[0], copy.size(), MSG_NOSIGNAL) > 0 &&
             send(fd, "\r\n", 2, MSG_NOSIGNAL) > 0;
    }
    __sync_fetch_and_sub(&s.clients, 1);
}

void* clientThread(void* arg) {
    int fd = (int)(intptr_t)arg;
    char req[512];
    ssize_t n = recv(fd, req, sizeof(req) - 1, 0);
    if (n > 0) {
        req[n] = 0;
        std::string line(req);
        line = line.substr(0, line.find('\r'));
        if (line.compare(0, 9, "GET /cam ") == 0) {
            streamTo(fd, g_cam);
        } else if (line.compare(0, 8, "GET /ui ") == 0) {
            streamTo(fd, g_ui);
        } else if (line.compare(0, 11, "GET /key?k=") == 0) {
            size_t b = 11, e = line.find(' ', b);
            bool ok = keysPushByName(line.substr(b, e == std::string::npos ? e : e - b));
            sendAll(fd, std::string("HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n") + (ok ? "ok" : "unknown key"));
        } else {
            sendAll(fd, std::string("HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n") + kPage);
        }
    }
    close(fd);
    return NULL;
}

void* serverThread(void*) {
    while (g_run) {
        int c = accept(g_listen, NULL, NULL);
        if (c < 0) continue;
        pthread_t t;
        if (pthread_create(&t, NULL, clientThread, (void*)(intptr_t)c) == 0) pthread_detach(t);
        else close(c);
    }
    return NULL;
}

}  // namespace

void webStart(int port) {
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(g_listen, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(g_listen, 4) != 0) {
        fprintf(stderr, "web: cannot listen on port %d\n", port);
        close(g_listen);
        g_listen = -1;
        return;
    }
    g_run = true;
    pthread_create(&g_serverThread, NULL, serverThread, NULL);
    pthread_detach(g_serverThread);
    printf("web: http://<board-ip>:%d/\n", port);
}

void webStop() {
    g_run = false;
    if (g_listen >= 0) shutdown(g_listen, SHUT_RDWR);
}

bool webWantsCam() { return g_cam.clients > 0; }
bool webWantsUi() { return g_ui.clients > 0; }
void webPublishCam(const cv::Mat& bgr) { publish(g_cam, bgr, 70); }

void webPublishUi(const cv::Mat& bgr) {
    cv::Mat big;
    cv::resize(bgr, big, cv::Size(), 2.0, 2.0, cv::INTER_NEAREST);
    publish(g_ui, big, 80);
}
