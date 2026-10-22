#ifndef WEB_H
#define WEB_H

#include <opencv2/core/core.hpp>

// Tiny HTTP server for bench work, no screen or keys needed:
//   /        page with the camera view, the screen picture and clickable keys
//   /cam     MJPEG camera view   /ui   MJPEG of what the LCD shows
//   /key?k=ok|up|down|left|right|adj   press a key from the browser
void webStart(int port);
void webStop();
bool webWantsCam();  // at least one browser is watching /cam
bool webWantsUi();
void webPublishCam(const cv::Mat& bgr);
void webPublishUi(const cv::Mat& bgr);

#endif // WEB_H
