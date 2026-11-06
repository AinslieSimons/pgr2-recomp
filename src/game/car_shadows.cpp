// CPU cars with the sun shadow of the player's car. PGR2 keeps one 1024x1024 depth map, of the car the camera follows
// (0x1892E0 on the shadow object 0x49E110), and draws one ground mesh with it (0x21920 on that car); every other car has only
// its texture blob (0x32AB0). Here the mesh pass also runs for each other car in view, with the map drawn for that car first,
// and the followed car's map is drawn again at the end for what reads it later in the frame. Each pass multiplies, so overlapping
// shadows would darken twice: src/game/draw_filter.cpp lets a pixel take one shadow. The first drawn wins and a shadow fades with
// its car's distance, so the followed car goes first (its map is already there) and the others nearest first.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

namespace pgr2 {
extern bool cpu_car_shadows;  // src/common/game_info.cpp
std::atomic<uint32_t> g_shadow_mark{1};  // the stencil value of this frame's sun shadows (src/game/draw_filter.cpp), 1 to 255
}

X86_EXTERN_FUNC(__imp__sub_001892E0);
X86_EXTERN_FUNC(__imp__sub_00021920);

namespace {
constexpr uint32_t kShadowObject = 0x49E110;
constexpr uint32_t kDrawMap = 0x1892E0;   // thiscall (car, strength); ret 8
constexpr uint32_t kEmptyMap = 0x188C20;   // thiscall: clears the map, no shadow from it
constexpr uint32_t kFullStrength = 0x275B98;  // the float 0x1D0C0 limits its strength to
constexpr uint32_t kCarDrawn = 0x1C900;   // thiscall on the car: al
constexpr uint32_t kInView = 0x184AD0;    // thiscall on the camera (position, radius, view): above -1 when in view
constexpr uint32_t kCamera = 0x3CD804;
constexpr uint32_t kCarList = 0x297390;   // the race's cars
constexpr uint32_t kCarCount = 0x0C, kCars = 0x1C00, kCarStride = 0x6990;
constexpr uint32_t kCarPosition = 0x44, kCarRadius = 0xA4, kCameraView = 0x18, kCameraPosition = 0xE0;

uint32_t g_followed = 0, g_strength = 0;  // the game's own call of the map this frame
bool g_busy = false;

float DistanceSquared(uint32_t a, uint32_t b) {
    float total = 0;
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t x = kernel::R32(a + i * 4), y = kernel::R32(b + i * 4);
        float fx, fy;
        std::memcpy(&fx, &x, 4);
        std::memcpy(&fy, &y, 4);
        total += (fx - fy) * (fx - fy);
    }
    return total;
}
}  // namespace

X86_FUNC(sub_001892E0) {
    if (!g_busy) {
        g_followed = kernel::R32(ctx.esp + 4);
        g_strength = kernel::R32(ctx.esp + 8);
    }
    __imp__sub_001892E0(ctx, base);
}

X86_FUNC(sub_00021920) {
    using kernel::R32;
    // In the hood and bumper views the game leaves the map out for the car it sits on (0x1D0C0 goes to 0x188C20, as 0x179E90 says
    // so), and still runs the pass: the other cars have their shadows there too, and the map is left empty again for what follows.
    const uint32_t followed = g_followed, own = ctx.ecx, strength = followed ? g_strength : R32(kFullStrength);
    g_followed = 0;  // drawn again next frame
    if (!pgr2::cpu_car_shadows || g_busy || !own || (followed && own != followed)) {
        __imp__sub_00021920(ctx, base);
        return;
    }
    pgr2::g_shadow_mark = pgr2::g_shadow_mark % 255 + 1;
    g_busy = true;
    __imp__sub_00021920(ctx, base);

    struct Other {
        uint32_t car;
        float distance;
    };
    static std::vector<Other> others;
    others.clear();
    const uint32_t count = R32(kCarList + kCarCount), camera = R32(kCamera);
    for (uint32_t k = 0; k < count; k++) {
        const uint32_t car = kCarList + kCars + k * kCarStride;
        if (car == own) continue;
        ctx.ecx = car;
        if (!(kernel::CallGuest(ctx, kCarDrawn, {}) & 0xFF)) continue;
        ctx.ecx = camera;
        if (int32_t(kernel::CallGuest(ctx, kInView, {car + kCarPosition, R32(car + kCarRadius), R32(camera + kCameraView)})) <= -1) continue;
        others.push_back({car, DistanceSquared(car + kCarPosition, camera + kCameraPosition)});
    }
    std::sort(others.begin(), others.end(), [](const Other& a, const Other& b) { return a.distance < b.distance; });
    for (const Other& other : others) {
        ctx.ecx = kShadowObject;
        kernel::CallGuest(ctx, kDrawMap, {other.car, strength});
        ctx.ecx = other.car;
        kernel::CallGuest(ctx, 0x21920, {});
    }
    if (!others.empty()) {
        ctx.ecx = kShadowObject;
        if (followed) kernel::CallGuest(ctx, kDrawMap, {followed, strength});
        else kernel::CallGuest(ctx, kEmptyMap, {});
    }
    static uint32_t passes = 0;
    if (++passes % 900 == 1) LOGR("car shadows: pass %u, %u cars, sun shadows on %zu others", passes, count, others.size());
    g_busy = false;
}
