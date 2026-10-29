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
    mode = clampInt(mode, 0, 2);
    contrast = clampInt(contrast, 5, 120);
    lang = clampInt(lang, 0, 1);
    shape = clampInt(shape, 30, 100);
    gain = (gain <= 0) ? 0 : clampInt(gain, 10, 640);
    roiW = clampInt(roiW, 32, 640);
    roiH = clampInt(roiH, 32, 480);
    roiX = clampInt(roiX, 0, 640 - roiW);
    roiY = clampInt(roiY, 0, 480 - roiH);
    tplScore = clampInt(tplScore, 30, 99);
    tplAngle = clampInt(tplAngle, 0, 10);
    tplShape = clampInt(tplShape, 0, 1);
    tplW = clampInt(tplW, 16, 400);
    tplH = clampInt(tplH, 16, 400);
    tplX = clampInt(tplX, 0, 640 - tplW);
    tplY = clampInt(tplY, 0, 480 - tplH);
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
        else if (!strcmp(key, "gain")) gain = v;
        else if (!strcmp(key, "mode")) mode = v;
        else if (!strcmp(key, "contrast")) contrast = v;
        else if (!strcmp(key, "lang")) lang = v;
        else if (!strcmp(key, "shape")) shape = v;
        else if (!strcmp(key, "roi_x")) roiX = v;
        else if (!strcmp(key, "roi_y")) roiY = v;
        else if (!strcmp(key, "roi_w")) roiW = v;
        else if (!strcmp(key, "roi_h")) roiH = v;
        else if (!strcmp(key, "tpl_score")) tplScore = v;
        else if (!strcmp(key, "tpl_angle")) tplAngle = v;
        else if (!strcmp(key, "tpl_shape")) tplShape = v;
        else if (!strcmp(key, "tpl_x")) tplX = v;
        else if (!strcmp(key, "tpl_y")) tplY = v;
        else if (!strcmp(key, "tpl_w")) tplW = v;
        else if (!strcmp(key, "tpl_h")) tplH = v;
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
    fprintf(f, "mode=%d\ntarget=%d\nmin_dia=%d\nmax_dia=%d\nroundness=%d\nedge=%d\ncontrast=%d\nshape=%d\n"
               "exposure=%d\ngain=%d\nroi_x=%d\nroi_y=%d\nroi_w=%d\nroi_h=%d\nlang=%d\n"
               "tpl_score=%d\ntpl_angle=%d\ntpl_shape=%d\ntpl_x=%d\ntpl_y=%d\ntpl_w=%d\ntpl_h=%d\n",
            mode, target, minDia, maxDia, roundness, edge, contrast, shape, exposure, gain, roiX, roiY, roiW, roiH, lang,
            tplScore, tplAngle, tplShape, tplX, tplY, tplW, tplH);
    fflush(f);
    fsync(fileno(f));
    bool ok = (fclose(f) == 0);
    return ok && rename(tmp, path) == 0;
}

SharedSettings::SharedSettings() : ver_(0), learnReq_(false), learnRes_(0), haveTpl_(false) { pthread_mutex_init(&m_, NULL); }

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

void SharedSettings::requestLearn() {
    pthread_mutex_lock(&m_);
    learnReq_ = true;
    pthread_mutex_unlock(&m_);
}

bool SharedSettings::takeLearn() {
    pthread_mutex_lock(&m_);
    bool r = learnReq_;
    learnReq_ = false;
    pthread_mutex_unlock(&m_);
    return r;
}

void SharedSettings::setLearnResult(int r) {
    pthread_mutex_lock(&m_);
    learnRes_ = r;
    pthread_mutex_unlock(&m_);
}

int SharedSettings::takeLearnResult() {
    pthread_mutex_lock(&m_);
    int r = learnRes_;
    learnRes_ = 0;
    pthread_mutex_unlock(&m_);
    return r;
}

void SharedSettings::setHaveTemplate(bool on) {
    pthread_mutex_lock(&m_);
    haveTpl_ = on;
    pthread_mutex_unlock(&m_);
}

bool SharedSettings::haveTemplate() {
    pthread_mutex_lock(&m_);
    bool r = haveTpl_;
    pthread_mutex_unlock(&m_);
    return r;
}
