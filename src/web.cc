#include "web.h"
#include "keys.h"
#include "settings.h"
#include "touch.h"
#include "webpage.h"

#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

volatile int g_rawWant = 0;  // /raw.png asks for one frame; the capture loop answers with webPublishRaw
std::vector<unsigned char> g_rawPng;

SharedSettings* g_settings = NULL;  // set by webSetControl: the page can read and change the settings
std::string g_confPath, g_tplPath;
pthread_mutex_t g_setLock = PTHREAD_MUTEX_INITIALIZER;  // one change at a time (read - modify - write)

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

void sendText(int fd, const char* type, const std::string& body, const char* status = "200 OK") {
    sendAll(fd, std::string("HTTP/1.0 ") + status + "\r\nCache-Control: no-cache\r\nContent-Type: " + type + "\r\n\r\n" + body);
}

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

// the settings as JSON, plus whether a template has been learned
std::string settingsJson() {
    Settings s = g_settings->get();
    std::string j = s.toJson();
    j.erase(j.size() - 1);  // the closing brace
    j += g_settings->haveTemplate() ? ",\"have_tpl\":1}" : ",\"have_tpl\":0}";
    return j;
}

// "GET /set?mode=2&tpl_score=80 HTTP/1.1": apply every key=value, keep the values inside their limits
void handleSet(int fd, const std::string& line) {
    size_t b = line.find('?'), e = line.find(' ', b == std::string::npos ? 0 : b);
    if (b == std::string::npos) { sendText(fd, "text/plain", "bad", "400 Bad Request"); return; }
    std::string q = line.substr(b + 1, e == std::string::npos ? e : e - b - 1);
    pthread_mutex_lock(&g_setLock);
    Settings s = g_settings->get();
    size_t i = 0;
    while (i < q.size()) {
        size_t amp = q.find('&', i);
        std::string pair = q.substr(i, amp == std::string::npos ? amp : amp - i);
        size_t eq = pair.find('=');
        if (eq != std::string::npos) s.setKey(pair.substr(0, eq).c_str(), atoi(pair.c_str() + eq + 1));
        if (amp == std::string::npos) break;
        i = amp + 1;
    }
    s.clamp();
    g_settings->set(s);
    pthread_mutex_unlock(&g_setLock);
    sendText(fd, "application/json", settingsJson());
}

void sendFile(int fd, const std::string& path, const char* type) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { sendText(fd, "text/plain", "no file", "404 Not Found"); return; }
    std::string body;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
    fclose(f);
    sendText(fd, type, body);
}

void* clientThread(void* arg) {
    int fd = (int)(intptr_t)arg;
    char req[2048];
    ssize_t n = recv(fd, req, sizeof(req) - 1, 0);
    if (n > 0) {
        req[n] = 0;
        std::string line(req);
        line = line.substr(0, line.find('\r'));
        if (line.compare(0, 9, "GET /cam ") == 0) {
            streamTo(fd, g_cam);
        } else if (line.compare(0, 8, "GET /ui ") == 0) {
            streamTo(fd, g_ui);
        } else if (line.compare(0, 13, "GET /touch?x=") == 0) {
            int tx = 0, ty = 0;
            bool ok = sscanf(line.c_str(), "GET /touch?x=%d&y=%d", &tx, &ty) == 2;
            if (ok) touchPush(tx, ty, true, false);
            sendText(fd, "text/plain", ok ? "ok" : "bad");
        } else if (line.compare(0, 11, "GET /key?k=") == 0) {
            size_t b = 11, e = line.find(' ', b);
            bool ok = keysPushByName(line.substr(b, e == std::string::npos ? e : e - b));
            sendText(fd, "text/plain", ok ? "ok" : "unknown key");
        } else if (line.compare(0, 9, "GET /raw.") == 0) {
            pthread_mutex_lock(&g_setLock);
            g_rawPng.clear();
            g_rawWant = 1;
            pthread_mutex_unlock(&g_setLock);
            for (int i = 0; i < 300 && g_rawWant; ++i) usleep(10000);
            pthread_mutex_lock(&g_setLock);
            std::string png(g_rawPng.begin(), g_rawPng.end());
            pthread_mutex_unlock(&g_setLock);
            if (png.empty()) sendText(fd, "text/plain", "no frame", "503 Service Unavailable");
            else sendText(fd, "image/png", png);
        } else if (g_settings && line.compare(0, 14, "GET /settings ") == 0) {
            sendText(fd, "application/json", settingsJson());
        } else if (g_settings && line.compare(0, 9, "GET /set?") == 0) {
            handleSet(fd, line);
        } else if (g_settings && line.compare(0, 11, "GET /learn ") == 0) {
            g_settings->requestLearn();
            sendText(fd, "text/plain", "ok");
        } else if (g_settings && line.compare(0, 10, "GET /save ") == 0) {
            pthread_mutex_lock(&g_setLock);
            bool ok = g_settings->get().save(g_confPath.c_str());
            pthread_mutex_unlock(&g_setLock);
            sendText(fd, "text/plain", ok ? "ok" : "failed");
        } else if (g_settings && line.compare(0, 13, "GET /tpl.png?") == 0) {
            sendFile(fd, g_tplPath, "image/png");
        } else {
            sendText(fd, "text/html; charset=utf-8", kPage);
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

void webSetControl(SharedSettings* settings, const std::string& confPath, const std::string& tplPath) {
    g_settings = settings;
    g_confPath = confPath;
    g_tplPath = tplPath;
}

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

bool webWantsRaw() { return g_rawWant != 0; }

void webPublishRaw(const cv::Mat& gray) {
    std::vector<unsigned char> png;
    cv::imencode(".png", gray, png);
    pthread_mutex_lock(&g_setLock);
    g_rawPng.swap(png);
    pthread_mutex_unlock(&g_setLock);
    g_rawWant = 0;
}

void webPublishUi(const cv::Mat& bgr) {
    cv::Mat big;
    cv::resize(bgr, big, cv::Size(), 2.0, 2.0, cv::INTER_NEAREST);
    publish(g_ui, big, 80);
}
