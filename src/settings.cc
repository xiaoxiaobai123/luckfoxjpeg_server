#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void Settings::clamp() {
    target = clampInt(target, 1, 30);
    minDia = clampInt(minDia, 4, 400);
    maxDia = clampInt(maxDia, minDia + 2, 600);
    roundness = clampInt(roundness, 5, 100);
    edge = clampInt(edge, 20, 255);
    exposure = clampInt(exposure, 0, 400);
}

bool Settings::load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char key[48];
        int v = 0;
        if (sscanf(line, "%47[^=]=%d", key, &v) != 2) continue;
        if (!strcmp(key, "target")) target = v;
        else if (!strcmp(key, "min_dia")) minDia = v;
        else if (!strcmp(key, "max_dia")) maxDia = v;
        else if (!strcmp(key, "roundness")) roundness = v;
        else if (!strcmp(key, "edge")) edge = v;
        else if (!strcmp(key, "exposure")) exposure = v;
    }
    fclose(f);
    clamp();
    return true;
}

bool Settings::save(const char* path) const {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "w");
    if (!f) return false;
    fprintf(f, "target=%d\nmin_dia=%d\nmax_dia=%d\nroundness=%d\nedge=%d\nexposure=%d\n",
            target, minDia, maxDia, roundness, edge, exposure);
    fflush(f);
    fsync(fileno(f));
    bool ok = (fclose(f) == 0);
    return ok && rename(tmp, path) == 0;
}

SharedSettings::SharedSettings() : ver_(0) { pthread_mutex_init(&m_, NULL); }

Settings SharedSettings::get() {
    pthread_mutex_lock(&m_);
    Settings s = s_;
    pthread_mutex_unlock(&m_);
    return s;
}

void SharedSettings::set(const Settings& s) {
    pthread_mutex_lock(&m_);
    s_ = s;
    s_.clamp();
    ++ver_;
    pthread_mutex_unlock(&m_);
}

unsigned SharedSettings::version() {
    pthread_mutex_lock(&m_);
    unsigned v = ver_;
    pthread_mutex_unlock(&m_);
    return v;
}
