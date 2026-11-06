// Car and mirror detail, more than the console's. The numbers are in FINDINGS.md, "Level of detail".
// A car's level (car +0x15D0, written by 0x3215D for each camera, read as it is drawn by 0x23020) picks one of up to 10 index
// sets over the same vertices, so a lower level costs triangles but no draws. It grows with distance / (viewport width x the
// float 0x2919C8), plus a term for the cars in view. The mirror's camera is 200 wide against the main view's 640, sees 500 against
// 3000 (the float 0x291A08) and draws each object to 0.3 of its range (cam +0x20).
// PGR2_DETAIL_AFTER=seconds (test): these take effect that long after the first race camera, to compare at one pose in one run.
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "kernel/kernel.h"

namespace pgr2 {
extern uint32_t car_detail;  // 0 the console's, 1 high (detail falls 4 times as slowly), 2 highest (always level 0)
extern bool mirror_detail;   // the mirror sees and draws as the main view does

namespace {
using kernel::G;
using kernel::R32;

constexpr uint32_t kCamera = 0x3CD804, kSizeFactor = 0x2919C8, kMirrorFar = 0x291A08, kRaceFar = 0x49DFDC;
constexpr uint32_t kCamKind = 0x40, kCamRange = 0x20, kCamFactor = 0x24, kCarLevel = 0x15D0, kMirror = 1;
constexpr float kConsoleSize = 11, kHighSize = 44;

void PutFloat(uint32_t va, float f) { std::memcpy(&G<uint32_t>(va), &f, 4); }
float Float(uint32_t va) {
    float f;
    std::memcpy(&f, &G<uint32_t>(va), 4);
    return f;
}

bool Active() {
    static const double after = std::getenv("PGR2_DETAIL_AFTER") ? std::atof(std::getenv("PGR2_DETAIL_AFTER")) : 0;
    if (after <= 0) return true;
    static const auto first = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - first).count() >= after;
}
uint32_t CarDetail() { return Active() ? car_detail : 0; }
bool MirrorDetail() { return Active() && mirror_detail; }
}  // namespace

// each frame
void ApplyDetail() { PutFloat(kSizeFactor, CarDetail() == 1 ? kHighSize : kConsoleSize); }

// before a car is drawn at its level, for the camera in use
void ApplyCarDetail(uint32_t car) {
    uint32_t detail = CarDetail();
    if (detail == 2) {
        G<uint8_t>(car + kCarLevel) = 0;
        return;
    }
    uint32_t cam = R32(kCamera);
    // about the main view's level for that distance: its viewport is 3.2 times as wide, at nearly the same scale
    if (cam && MirrorDetail() && R32(cam + kCamKind) == kMirror) G<uint8_t>(car + kCarLevel) = G<uint8_t>(car + kCarLevel) / 3;
}

// around each race camera's set up (0x1859A0)
void MirrorFarBefore(uint32_t cam) {
    static float console = 0, written = 0;
    if (R32(cam + kCamKind) != kMirror) return;
    float cur = Float(kMirrorFar);
    if (cur != written) console = cur;  // the game's own, set at race setup
    written = MirrorDetail() ? Float(kRaceFar) : console;
    PutFloat(kMirrorFar, written);
}
void MirrorRangeAfter(uint32_t cam) {
    if (R32(cam + kCamKind) == kMirror && MirrorDetail()) PutFloat(cam + kCamRange, Float(cam + kCamFactor));
}
}  // namespace pgr2
