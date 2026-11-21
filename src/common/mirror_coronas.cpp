// The rear view mirror's coronas at the main view's reach ([[hook]] before 0x41951 in config/pgr2.toml; PGR2_MIRROR_CORONA_REACH=0
// as the console, =N N times the main view's reach).
//
// 0x415B0 sizes a car lamp's corona as the view's box width (+0x30) x its projection's x scale (+0x130) x the lamp's size over its
// distance, and draws it only while that is over 1.0. The mirror's box is 200 units wide (167 on a 16:10 frame, hooks.cpp narrows
// it) against the main view's 640, with a larger x scale: its coronas are about a third the main view's size, so they went out at
// about a third of the distance, while the car and its lamps were still plainly in the mirror. Here a mirror corona the main view
// would still draw at that distance is kept, at the least size that passes. It lives in the host (not src/game): on Windows the
// game's code is a DLL that imports its hooks from the executable.
#include <cstdlib>
#include <cstring>
#include <cmath>

#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

namespace {
constexpr uint32_t kCamera = 0x3CD804;      // the camera being drawn
constexpr uint32_t kMainCamera = 0x3B59C4;  // the camera holder's first: the main view
constexpr uint32_t kKind = 0x40, kWidth = 0x30, kProjection = 0x130;
constexpr float kLeast = 1.0001f;  // just over the test's 1.0

float F(uint32_t va) {
    const uint32_t v = kernel::R32(va);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
}  // namespace

// before 0x41951: xmm0 (and [esp+0x10], which the quad is drawn from) the corona's size
X86_HOST_API void MirrorCoronaReach(X86Context& ctx, uint8_t* base) {
    (void)base;
    static const bool off = [] {  // PGR2_MIRROR_CORONA_REACH=0: as the console
        const char* e = std::getenv("PGR2_MIRROR_CORONA_REACH");
        return e && *e == '0';
    }();
    const float size = ctx.xmm[0].f[0];
    const uint32_t cam = kernel::R32(kCamera), main = kernel::R32(kMainCamera);
    if (off || !(size > 0.0f) || size > 1.0f) return;
    if (!cam || !main || cam == main || kernel::R32(cam + kKind) != 1) return;
    const float mirror = float(int32_t(kernel::R32(cam + kWidth))) * std::fabs(F(cam + kProjection));
    const float full = float(int32_t(kernel::R32(main + kWidth))) * std::fabs(F(main + kProjection));
    if (!(mirror > 0.0f) || full <= mirror) return;
    static const float reach = [] {  // PGR2_MIRROR_CORONA_REACH=N: N times the main view's reach (1 by default)
        const char* e = std::getenv("PGR2_MIRROR_CORONA_REACH");
        return e && *e && *e != '0' ? float(std::atof(e)) : 1.0f;
    }();
    if (size * reach * full / mirror <= 1.0f) return;  // the main view would not draw it either
    ctx.xmm[0].f[0] = kLeast;
    uint32_t v;
    std::memcpy(&v, &kLeast, 4);
    kernel::W32(ctx.esp + 0x10, v);
}
