// Planar reflections on water, which the console never had: it reflects a fixed cube of each city (the bump-mapped shader with
// texture modes 1, 0x11, 0x11, 0xC; water is the flat use of it, y -0.177 in Sydney, 2.0 in Hong Kong, -5.497 in city 4; FINDINGS.md,
// "Reflections"). Here the main camera is copied each frame, mirrored in the water's plane and drawn whole into a 640x480 target,
// and water draws read that target at their own screen position, bent by their normal map (gpu/shaders.cpp, mode 0xC with a 2D
// texture in stage 3, which a console draw cannot be).
// The copy runs from the hook on the camera's draw (0x17FCC0), as the viewer's mirror camera does: same class, its +0x14 set for the
// flipped winding. It only runs while water reached the renderer in the last half second, so a street with none costs nothing.
// What lies under the plane is clipped with an oblique near plane: the projection's z becomes a tiny multiple of the height above
// the water. PGR2 draws with a W-buffer, so depth is unaffected and z is left only deciding the clip.
// The draw filter runs on the render thread and only reads what the hook publishes; a frame late is harmless.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

#include "app/app.h"
#include "app/perf.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "gpu/backend.h"
#include "gpu/state.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace pgr2 {
extern bool planar_water;  // src/common/game_info.cpp
extern bool water_area_cull;  // src/common/game_info.cpp
extern uint32_t water_scale;  // src/common/game_info.cpp: the reflection's size, percent of the frame's
extern uint32_t water_far_detail;  // src/common/game_info.cpp: which far, small blocks the reflection leaves out
extern uint32_t race_fps;  // src/common/game_info.cpp
extern uint32_t water_update;  // src/common/game_info.cpp: 0 every frame, 1 every other frame while over budget, 2 always every other frame
}

namespace {
using kernel::G;
using kernel::R32;
using kernel::W32;

constexpr uint32_t kHolder = 0x3B59C0, kHolderCameras = 0x04, kHolderActive = 0x17E44;
constexpr uint32_t kCameraBytes = 0xDE90;  // the array's stride (0xF6A90), what 0xF6340 constructs
constexpr uint32_t kCamTests = 0x1F0, kCamTestsEnd = 0xBD74;  // 0xFA0 slot pointers, then 0xFA0 tests at +0x4074 (0x5A290)
constexpr uint32_t kConstruct = 0xF6340;           // thiscall: a race camera, as 0xF6AF2 builds them
constexpr uint32_t kCamMirrored = 0x14, kCamWidth = 0x30, kCamHeight = 0x34, kCamKind = 0x40, kCamPosition = 0xE0, kCamView = 0xF0,
                   kCamProjection = 0x130, kCamClear = 0xBEF0;
constexpr uint32_t kCreateSurface = 0x1C4AA0;      // stdcall (width, height, 1, format), as 0x178A1F
constexpr uint32_t kSetTextureHeader = 0x1E0645;   // XGSetTextureHeader, as 0x178A3E
constexpr uint32_t kResourceRelease = 0x1C31F0;    // stdcall (resource), as 0x17883B
constexpr uint32_t kSurfaceHeader = 0x18;
constexpr uint32_t kSetRenderTarget = 0x1C3980;    // D3DDevice_SetRenderTarget (colour, depth)
constexpr uint32_t kFrameColour = 0x485DFC, kFrameDepth = 0x485E00;
constexpr uint32_t kBuildView = 0x188550;          // thiscall, the camera's vtable +0x1C: its view for this frame
constexpr uint32_t kEmptyTests = 0x187D90;         // thiscall: waits for a camera's tests and frees their slots
constexpr uint32_t kFrustum = 0x187BD0;            // thiscall: the camera's planes from its view and projection
constexpr uint32_t kShadowMapOn = 0x49E118;        // 1: the camera's draw makes the sun shadow map (0x17FDBD)
constexpr uint32_t kDepthOfField = 0x291A18;       // above 0: the camera's draw blurs the frame (0x17FF36)
constexpr uint32_t kWaterModes = 0x64621;
constexpr uint32_t kFormatColour = 0x12, kFormatDepth = 0x2E;  // LIN_A8R8G8B8, LIN_D24S8
constexpr uint32_t kArena = 0x70000000;            // past full_textures.cpp's
constexpr uint32_t kFramesKept = 30;               // the pass stays on this long after the last water draw
constexpr float kClipSlope = 1e-3f;                // z per metre above the water, small enough that z stays under w
constexpr float kClipBelow = 0.05f;                // the clip plane sits this far under the water, so that banks meet it
constexpr float kPlaneNear = 0.05f;                // a water draw this close to the plane reflects in it

uint32_t g_camera = 0, g_header = 0, g_colour = 0, g_depth = 0;
bool g_busy = false, g_failed = false;
uint32_t g_frame = 0;

std::atomic<uint32_t> g_target_data{0};  // the reflection's colour data, as a draw's Surface holds it
std::atomic<uint32_t> g_texture{0};      // its texture header, 0 while no reflection is current
std::atomic<float> g_plane{0};           // the plane the current reflection was drawn for
std::atomic<uint32_t> g_water_frame{0};  // g_frame when the renderer last saw water
std::atomic<float> g_water_y{0};         // the level of the largest water seen

// Water-area culling. A water pixel reads the reflection at its own place on the screen, so what lands outside the part of
// the screen that has water is never seen, and need not be drawn. The bounds of last frame's water, in x / z and y / z of the
// mirrored camera (a point on the plane is where it was), go to the game's two culling tests below for this camera. The
// margins are for the ripple, which bends a read by up to about that much of the screen, and for a frame's movement.
constexpr float kAreaMarginX = 0.16f, kAreaMarginY = 0.40f;  // of the screen's half width and height
constexpr float kAreaHuge = 1e6f;
std::atomic<float> g_area[4];            // x0, x1, y0, y1
std::atomic<uint32_t> g_area_frame{0};   // g_frame they were gathered in, 0 none
std::atomic<float> g_area_view[12];      // the mirrored view's three rows of x, y, z, as WaterDraw reads them
float g_scale_x = 1, g_scale_y = 1;      // the projection's, for the margins

using Matrix = float[16];
void Read(uint32_t va, Matrix m) { std::memcpy(m, &G<uint8_t>(va), sizeof(Matrix)); }
void Write(uint32_t va, const Matrix m) { std::memcpy(&G<uint8_t>(va), m, sizeof(Matrix)); }
void Multiply(const Matrix a, const Matrix b, Matrix out) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = v;
        }
}
float F32(uint32_t va) {
    float f;
    std::memcpy(&f, &G<uint8_t>(va), 4);
    return f;
}
void WF32(uint32_t va, float f) { std::memcpy(&G<uint8_t>(va), &f, 4); }

// a surface like the one D3D makes, its pixels in our own memory: this can first run in the middle of a city's load, and the game's
// 64 MB is tight enough without 2.4 MB more. 0x1C4AA0 makes a real one for its Format and Size words, and it goes straight back.
uint32_t OwnSurface(X86Context& ctx, uint32_t format, uint32_t at, uint32_t data) {
    const uint32_t made = kernel::CallGuest(ctx, kCreateSurface, {640, 480, 1, format});
    if (!made) return 0;
    std::memcpy(&G<uint8_t>(at), &G<uint8_t>(made), kSurfaceHeader);
    W32(at + 4, data);
    kernel::CallGuest(ctx, kResourceRelease, {made});
    return at;
}

bool Ensure(X86Context& ctx) {
    if (g_camera) return true;
    if (g_failed) return false;
    g_failed = true;  // once: a failed allocation is not asked again
    const uint32_t header = kArena + kCameraBytes, colour = header + 0x40, depth = header + 0x80, pixels = (header + 0xFFF + 0x100) & ~0xFFFu,
                   bytes = 640 * 480 * 4, end = pixels + 2 * bytes;
    if (!mem::Commit(kArena, end - kArena)) {
        LOGR("planar water: no room at %08X", kArena);
        return false;
    }
    g_colour = OwnSurface(ctx, kFormatColour, colour, pixels);
    g_depth = OwnSurface(ctx, kFormatDepth, depth, pixels + bytes);
    if (!g_colour || !g_depth) {
        LOGR("planar water: no surfaces (%08X, %08X)", g_colour, g_depth);
        return false;
    }
    g_header = header;
    kernel::CallGuest(ctx, kSetTextureHeader, {640, 480, 1, 0, kFormatColour, 0, g_header, pixels, 0});
    ctx.ecx = kArena;
    kernel::CallGuest(ctx, kConstruct, {});
    g_camera = kArena;
    g_failed = false;
    g_target_data = pixels;
    gpu::g_small_target[0] = pixels, gpu::g_small_target[1] = pixels + bytes;
    LOGR("planar water: target %08X depth %08X data %08X", g_colour, g_depth, pixels);
    return true;
}

// V' = R V, R the mirror in y = plane
void MirrorView(const Matrix view, float plane, Matrix out) {
    const Matrix r = {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 2 * plane, 0, 1};
    Multiply(r, view, out);
}

// a rigid view's inverse, mirrored or not: the rotation's transpose, scaled by nothing
void InvertRigid(const Matrix m, Matrix out) {
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) out[r * 4 + c] = m[c * 4 + r];
        out[r * 4 + 3] = 0;
    }
    for (int c = 0; c < 3; c++) out[12 + c] = -(m[12] * out[c] + m[13] * out[4 + c] + m[14] * out[8 + c]);
    out[15] = 1;
}

// the mirror's copy of the camera, drawn into the target. Its view goes in from the hook on 0x188550, where the draw builds a camera's.
Matrix g_mirrored;
float g_mirror_plane = 0;

void DrawReflection(X86Context& ctx, uint32_t main, float plane) {
    ctx.ecx = main;
    kernel::CallGuest(ctx, kBuildView, {});  // this frame's view; the draw itself only builds it later
    // all but its tables of visibility tests: their slots come from one pool (0x40AD48) and each camera owns its own, as the mirror's does
    std::memcpy(&G<uint8_t>(g_camera), &G<uint8_t>(main), kCamTests);
    std::memcpy(&G<uint8_t>(g_camera + kCamTestsEnd), &G<uint8_t>(main + kCamTestsEnd), kCameraBytes - kCamTestsEnd);
    Matrix view;
    Read(main + kCamView, view);
    MirrorView(view, plane, g_mirrored);
    g_mirror_plane = plane;
    for (int i = 0; i < 12; i++) g_area_view[i].store(g_mirrored[i / 3 * 4 + i % 3], std::memory_order_relaxed);
    g_scale_x = std::fabs(F32(main + kCamProjection)), g_scale_y = std::fabs(F32(main + kCamProjection + 0x14));
    G<uint8_t>(g_camera + kCamMirrored) = G<uint8_t>(main + kCamMirrored) ? 0 : 1;
    G<uint8_t>(g_camera + kCamClear) = 1;  // clears with the fog colour; the sky then draws over it

    const uint32_t active = R32(kHolder + kHolderActive), shadow = R32(kShadowMapOn), blur = R32(kDepthOfField);
    kernel::CallGuest(ctx, kSetRenderTarget, {g_colour, g_depth});
    W32(kHolder + kHolderActive, g_camera);
    W32(kShadowMapOn, 0);
    W32(kDepthOfField, 0);
    ctx.ecx = g_camera;
    kernel::CallGuest(ctx, 0x17FCC0, {});
    W32(kDepthOfField, blur);
    W32(kShadowMapOn, shadow);
    W32(kHolder + kHolderActive, active);
    kernel::CallGuest(ctx, kSetRenderTarget, {R32(kFrameColour), R32(kFrameDepth)});
}

void SetMirrorView(X86Context& ctx) {
    Write(g_camera + kCamView, g_mirrored);
    WF32(g_camera + kCamPosition + 4, 2 * g_mirror_plane - F32(g_camera + kCamPosition + 4));
    ctx.ecx = g_camera;
    kernel::CallGuest(ctx, kFrustum, {});
    // z = slope * (height over the plane), in terms of the mirrored view's coordinates; column 2 of the projection, row vectors
    Matrix back;
    InvertRigid(g_mirrored, back);
    const float height[4] = {back[1], back[5], back[9], back[13] - (g_mirror_plane - kClipBelow)};
    for (int row = 0; row < 4; row++) WF32(g_camera + kCamProjection + (row * 4 + 2) * 4, kClipSlope * height[row]);
}

// PGR2_WATER_CAM="x y z yaw pitch" (test, degrees): the main camera stands there, to look at water no road passes
void TestCamera(X86Context& ctx, uint32_t camera) {
    static const char* const e = std::getenv("PGR2_WATER_CAM");
    if (!e) return;
    float x, y, z, yaw, pitch;
    if (std::sscanf(e, "%f %f %f %f %f", &x, &y, &z, &yaw, &pitch) != 5) return;
    const float a = yaw * 3.14159265f / 180, b = pitch * 3.14159265f / 180;
    const float f[3] = {std::sin(a) * std::cos(b), std::sin(b), std::cos(a) * std::cos(b)};
    const float r[3] = {std::cos(a), 0, -std::sin(a)};
    const float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    const Matrix v = {r[0], u[0], f[0], 0, r[1], u[1], f[1], 0, r[2], u[2], f[2], 0,
                      -(x * r[0] + y * r[1] + z * r[2]), -(x * u[0] + y * u[1] + z * u[2]), -(x * f[0] + y * f[1] + z * f[2]), 1};
    Write(camera + kCamView, v);
    WF32(camera + kCamPosition, x);
    WF32(camera + kCamPosition + 4, y);
    WF32(camera + kCamPosition + 8, z);
    ctx.ecx = camera;
    kernel::CallGuest(ctx, kFrustum, {});
}

}  // namespace

// a race's end empties the main camera's tests (its vtable +0xC, 0x187DC0): the reflection's go back to the pool with them
X86_EXTERN_FUNC(__imp__sub_00187DC0);
X86_FUNC(sub_00187DC0) {
    const uint32_t camera = ctx.ecx;
    __imp__sub_00187DC0(ctx, base);
    if (g_camera && camera != g_camera && camera == R32(kHolder + kHolderCameras)) {
        ctx.ecx = g_camera;
        kernel::CallGuest(ctx, kEmptyTests, {});
        ctx.ecx = camera;
    }
}

// what a camera's draw builds its view with
X86_EXTERN_FUNC(__imp__sub_00188550);
X86_FUNC(sub_00188550) {
    const uint32_t camera = ctx.ecx;
    if (camera && camera == g_camera) {
        SetMirrorView(ctx);
        ctx.ecx = camera;
        ctx.esp += 4;  // ret: the caller's stack as the function's own return leaves it
        return;
    }
    __imp__sub_00188550(ctx, base);
    if (camera == R32(kHolder + kHolderCameras)) {
        TestCamera(ctx, camera);
        ctx.ecx = camera;
    }
}

namespace {
// the performance recorder's objects.csv: every candidate the game's culling tests see, counted by camera, test, result and
// bands of size, distance and draw range, every half second. ratio is (distance - radius) / the object's draw range, so the
// effect of a shorter range can be read off: an object goes at a range factor below its ratio.
constexpr int kCams = 5, kTests = 2, kResults = 2, kRadii = 8, kDists = 9, kRanges = 8, kRatios = 8;
const float kRadiusEdges[kRadii - 1] = {1, 2, 5, 10, 20, 50, 100};
const float kDistEdges[kDists - 1] = {25, 50, 100, 200, 400, 800, 1600, 3200};
const float kRangeEdges[kRanges - 1] = {50, 100, 200, 400, 800, 1600, 3200};
const float kRatioEdges[kRatios - 1] = {0.1f, 0.2f, 0.3f, 0.5f, 0.75f, 1.0f, 1.5f};
std::atomic<uint32_t> g_objects[kCams * kTests * kResults * kRadii * kDists * kRanges * kRatios];
template <int N> int Band(const float (&edges)[N], float v) {
    int i = 0;
    while (i < N && v >= edges[i]) i++;
    return i;
}
// 0 main, 1 mirror, 2 reflection cube, 3 water reflection, 4 another
int CamClass(uint32_t camera) {
    if (camera == g_camera && g_camera) return 3;
    const uint32_t kind = R32(camera + kCamKind);
    return kind <= 2 ? int(kind) : 4;
}
void Tally(uint32_t camera, int test, bool outside, float radius, float distance, float range) {
    const int ratio = range > 0 ? Band(kRatioEdges, std::max(distance - radius, 0.0f) / range) : 0, rng = range > 0 ? Band(kRangeEdges, range) : 0;
    const int at = (((((CamClass(camera) * kTests + test) * kResults + (outside ? 1 : 0)) * kRadii + Band(kRadiusEdges, radius)) * kDists + Band(kDistEdges, distance)) * kRanges + rng) * kRatios + ratio;
    g_objects[at].fetch_add(1, std::memory_order_relaxed);
}
float EyeDistance(uint32_t camera, float x, float y, float z) {
    const float dx = x - F32(camera + kCamPosition), dy = y - F32(camera + kCamPosition + 4), dz = z - F32(camera + kCamPosition + 8);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
void FlushObjects() {
    if (!perf::On()) return;
    std::string rows;
    char line[96];
    const double t = perf::ElapsedMs();
    for (size_t i = 0; i < std::size(g_objects); i++) {
        const uint32_t n = g_objects[i].exchange(0, std::memory_order_relaxed);
        if (!n) continue;
        size_t v = i;
        const int ratio = int(v % kRatios); v /= kRatios;
        const int range = int(v % kRanges); v /= kRanges;
        const int dist = int(v % kDists); v /= kDists;
        const int radius = int(v % kRadii); v /= kRadii;
        const int out = int(v % kResults); v /= kResults;
        const int test = int(v % kTests); v /= kTests;
        std::snprintf(line, sizeof line, "%.0f,%d,%d,%d,%d,%d,%d,%d,%u\n", t, int(v), test, out, radius, dist, range, ratio, n);
        rows += line;
    }
    perf::ObjectRows(rows);
}

// the area for this camera, widened by the margins; false to leave the camera's own test alone
bool CullArea(uint32_t camera, float area[4]) {
    if (!pgr2::water_area_cull || camera != g_camera || !g_camera || !g_busy) return false;
    const uint32_t frame = g_area_frame.load(std::memory_order_acquire);
    if (!frame || g_frame - frame > 2) return false;
    const float mx = kAreaMarginX / std::max(g_scale_x, 0.01f), my = kAreaMarginY / std::max(g_scale_y, 0.01f);
    area[0] = g_area[0].load(std::memory_order_relaxed) - mx, area[1] = g_area[1].load(std::memory_order_relaxed) + mx;
    area[2] = g_area[2].load(std::memory_order_relaxed) - my, area[3] = g_area[3].load(std::memory_order_relaxed) + my;
    return true;
}
}  // namespace

// the sphere test (thiscall (position, radius, range), ret 0xC; -1 outside): objects, trees, cars
X86_EXTERN_FUNC(__imp__sub_00184AD0);
X86_FUNC(sub_00184AD0) {
    float a[4];
    const bool logging = perf::On();
    const uint32_t tested = ctx.ecx;
    float log_r = 0, log_d = 0, log_range = 0;
    if (logging) {
        const uint32_t q = R32(ctx.esp + 4);
        log_r = F32(ctx.esp + 8), log_range = F32(ctx.esp + 12), log_d = EyeDistance(tested, F32(q), F32(q + 4), F32(q + 8));
    }
    if (CullArea(ctx.ecx, a)) {
        const uint32_t p = R32(ctx.esp + 4);
        const float r = F32(ctx.esp + 8), x = F32(p), y = F32(p + 4), z = F32(p + 8);
        const float cx = g_mirrored[0] * x + g_mirrored[4] * y + g_mirrored[8] * z + g_mirrored[12];
        const float cy = g_mirrored[1] * x + g_mirrored[5] * y + g_mirrored[9] * z + g_mirrored[13];
        const float cz = g_mirrored[2] * x + g_mirrored[6] * y + g_mirrored[10] * z + g_mirrored[14];
        const float side[4] = {(a[0] * cz - cx) / std::sqrt(1 + a[0] * a[0]), (cx - a[1] * cz) / std::sqrt(1 + a[1] * a[1]),
                               (a[2] * cz - cy) / std::sqrt(1 + a[2] * a[2]), (cy - a[3] * cz) / std::sqrt(1 + a[3] * a[3])};
        for (float d : side)
            if (d > r) {
                ctx.eax = 0xFFFFFFFFu;
                ctx.esp += 16;
                if (logging) Tally(tested, 0, true, log_r, log_d, log_range);
                return;
            }
    }
    __imp__sub_00184AD0(ctx, base);
    if (logging) Tally(tested, 0, ctx.eax == 0xFFFFFFFFu, log_r, log_d, log_range);
}

// whether the water's reflection leaves this block out: far from the eye and small. Big blocks stay, so the skyline does, and the
// quadtree walk (0x119430) then tests their children. Limits from a Hong Kong race's objects.csv: FINDINGS.md, "Water reflections".
bool FarAndSmall(uint32_t camera, uint32_t box) {
    static constexpr struct { float distance, radius; } kLimits[] = {{800, 20}, {400, 50}, {400, 100}};
    if (camera != g_camera || !g_camera || !g_busy || !pgr2::water_far_detail) return false;
    const auto& limit = kLimits[std::min<uint32_t>(pgr2::water_far_detail, std::size(kLimits)) - 1];
    const float hx = F32(box + 0x10), hy = F32(box + 0x14), hz = F32(box + 0x18);
    return hx * hx + hy * hy + hz * hz < limit.radius * limit.radius && EyeDistance(camera, F32(box), F32(box + 4), F32(box + 8)) > limit.distance;
}

// the box test (thiscall (box, planes), ret 8; -1 outside): the city's blocks. The box: centre at +0, half sizes at +0x10
X86_EXTERN_FUNC(__imp__sub_00184F10);
X86_FUNC(sub_00184F10) {
    float a[4];
    const bool logging = perf::On();
    const uint32_t tested = ctx.ecx;
    float log_r = 0, log_d = 0;
    if (logging) {
        const uint32_t q = R32(ctx.esp + 4);
        const float hx = F32(q + 0x10), hy = F32(q + 0x14), hz = F32(q + 0x18);
        log_r = std::sqrt(hx * hx + hy * hy + hz * hz), log_d = EyeDistance(tested, F32(q), F32(q + 4), F32(q + 8));
    }
    if (FarAndSmall(tested, R32(ctx.esp + 4))) {
        ctx.eax = 0xFFFFFFFFu;
        ctx.esp += 12;
        if (logging) Tally(tested, 1, true, log_r, log_d, 0);
        return;
    }
    if (CullArea(ctx.ecx, a)) {
        const uint32_t b = R32(ctx.esp + 4);
        const float c[3] = {F32(b), F32(b + 4), F32(b + 8)}, h[3] = {F32(b + 0x10), F32(b + 0x14), F32(b + 0x18)};
        const float* m = g_mirrored;
        auto outside = [&](int row, float bound, float sign) {  // sign (row - bound z) > 0 for the whole box
            float d = sign * (m[12 + row] - bound * m[14]), r = 0;
            for (int i = 0; i < 3; i++) {
                const float n = sign * (m[i * 4 + row] - bound * m[i * 4 + 2]);
                d += n * c[i], r += std::fabs(n) * h[i];
            }
            return d - r > 0;
        };
        if (outside(0, a[0], -1) || outside(0, a[1], 1) || outside(1, a[2], -1) || outside(1, a[3], 1)) {
            ctx.eax = 0xFFFFFFFFu;
            ctx.esp += 12;
            if (logging) Tally(tested, 1, true, log_r, log_d, 0);
            return;
        }
    }
    __imp__sub_00184F10(ctx, base);
    if (logging) Tally(tested, 1, ctx.eax == 0xFFFFFFFFu, log_r, log_d, 0);
}

namespace pgr2 {

// from the race view's draw (0x17FCC0, src/game/hooks.cpp), before it draws, ecx the camera
// whether this frame keeps the reflection of the frame before. Frames are timed in pairs on the game thread: at half rate a frame
// that draws the reflection runs long and the one that keeps it short, however steady the display is, so a single frame's time says
// nothing. Pairs do not overlap, so one hitch is at most two slow pairs; three slow pairs of the last four (5% past the frame's
// target, so a frame capped at 30 is on time) start the half rate, and a long gap (loading, a pause) is not a slow frame. Nothing
// measured at half rate says whether a full rate would fit, so it ends after a while and tries: slow again soon after doubles the
// wait, up to 16 seconds at 60 fps, and a quiet five seconds starts again from two.
static uint32_t g_drawn_frame = 0;
static bool KeepLastReflection(float plane) {
    using Clock = std::chrono::steady_clock;
    constexpr double kOverBudget = 1.05, kNotAFrameMs = 100;
    constexpr uint32_t kSlowPairs = 3, kShortestHold = 120, kLongestHold = 960, kSettledFrames = 300;
    static Clock::time_point last;
    static double last_ms = 0;
    static uint32_t hold = 0, hold_length = kShortestHold, settled = kSettledFrames, history = 0, frames = 0;
    const auto now = Clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    const app::Settings& s = app::Get();
    const uint32_t fps = s.fps_cap ? std::min(s.fps_cap, race_fps) : race_fps;
    if (++frames % 2 == 0) {
        const bool slow = fps && ms < kNotAFrameMs && last_ms < kNotAFrameMs && (ms + last_ms) / 2 > kOverBudget * 1000.0 / fps;
        history = (history << 1 | uint32_t(slow)) & 0xF;
    }
    last_ms = ms;
    if (hold) hold--;
    else if (settled < kSettledFrames) settled++;
    if (uint32_t(__builtin_popcount(history)) >= kSlowPairs) {
        if (!hold) {
            hold_length = settled < kSettledFrames ? std::min(hold_length * 2, kLongestHold) : kShortestHold;
            settled = 0;
        }
        hold = hold_length;
    }
    const bool half = water_update >= 2 || (water_update == 1 && hold);
    return half && g_drawn_frame && g_drawn_frame + 1 == g_frame && g_plane.load(std::memory_order_relaxed) == plane;
}

void WaterBeforeView(X86Context& ctx) {
    const uint32_t camera = ctx.ecx;
    if (g_busy || camera != R32(kHolder + kHolderCameras) || R32(camera + kCamKind) != 0 || G<uint8_t>(camera + kCamMirrored) ||
        R32(camera + kCamWidth) != 640 || R32(camera + kCamHeight) != 480) return;
    g_frame++;
    if (g_frame % 30 == 0) FlushObjects();
    gpu::g_small_target_factor.store(std::clamp<uint32_t>(water_scale, 25, 100) / 100.0f, std::memory_order_relaxed);
    const bool wanted = planar_water && g_water_frame.load(std::memory_order_relaxed) + kFramesKept > g_frame;
    uint32_t texture = 0;
    if (wanted && Ensure(ctx)) {
        const float plane = g_water_y.load(std::memory_order_relaxed);
        // from above only: under the plane there is nothing to mirror
        if (F32(camera + kCamPosition + 4) > plane) {
            if (!KeepLastReflection(plane)) {
                g_busy = true;
                DrawReflection(ctx, camera, plane);
                g_busy = false;
                g_plane = plane;
                g_drawn_frame = g_frame;
                static uint32_t passes = 0;
                if (++passes % 300 == 1) LOGR("planar water: pass %u, level %.3f, camera at %.1f %.1f %.1f", passes, plane, F32(camera + kCamPosition), F32(camera + kCamPosition + 4), F32(camera + kCamPosition + 8));
            }
            texture = g_header;
        }
    }
    g_texture = texture;
    ctx.ecx = camera;
}

}  // namespace pgr2

namespace pgr2 {

// the draw filter's part: a water draw reads the reflection in place of its cube
// a draw into the water's reflection: the scene drawn mirrored in the water's plane (src/game/car_reflections.cpp leaves it
// out of the traced scene)
bool InWaterReflection(const gpu::State& s) {
    const uint32_t target = g_target_data.load(std::memory_order_relaxed);
    return target && s.rt.data == target;
}

void WaterRegisterLog() {
    perf::SetObjectsHeader("t_ms,camera,test,outside,radius_band,distance_band,range_band,ratio_band,count");
}

void WaterDraw(const gpu::State& s, gpu::DrawAdjust& adjust) {
    if (s.rs[gpu::kRsPsTextureModes] != kWaterModes || !planar_water) return;
    if (s.rt.data && s.rt.data == g_target_data.load(std::memory_order_relaxed)) {
        adjust.skip = true;  // on the plane itself: nothing to see in its own mirror
        return;
    }
    if (s.viewport.w != 640 || s.viewport.h != 480 || !s.vs || !s.streams[0].vb || s.vs->inputs[0].stream != 0 || s.vs->inputs[0].type != 0x32) return;
    if (adjust.count < 3) return;
    // level over the whole draw: water, not a bump-mapped wall
    const uint32_t vb = mem::VirtualFromPhysical(R32(s.streams[0].vb + 4)), stride = s.streams[0].stride, at = s.vs->inputs[0].offset;
    float low = 1e30f, high = -1e30f, x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
    for (uint32_t i = 0; i < adjust.count; i++) {
        const uint32_t v = adjust.indices ? (uint32_t(adjust.indices[i * 2]) | uint32_t(adjust.indices[i * 2 + 1]) << 8) + s.base_vertex : adjust.start + i;
        const uint32_t p = vb + v * stride + at;
        const float x = F32(p), y = F32(p + 4), z = F32(p + 8);
        x0 = std::min(x0, x), x1 = std::max(x1, x);
        low = std::min(low, y), high = std::max(high, y);
        z0 = std::min(z0, z), z1 = std::max(z1, z);
    }
    if (!(high - low < 0.01f)) return;

    // the plane is the level with the largest draw seen lately, so a pond doesn't take the harbour's
    static float best_area = 0, best_y = 0;
    static uint32_t best_frame = 0;
    const uint32_t frame = g_frame;
    const float area = (x1 - x0) * (z1 - z0);
    if (frame != best_frame && frame % 16 == 0) best_area = 0, best_frame = frame;
    if (area >= best_area || std::fabs(low - best_y) < kPlaneNear) {
        if (area >= best_area) best_area = area;
        best_y = low;
        g_water_y.store(low, std::memory_order_relaxed);
    }
    g_water_frame.store(frame, std::memory_order_relaxed);

    const uint32_t texture = g_texture.load(std::memory_order_relaxed);
    if (!texture || std::fabs(low - g_plane.load(std::memory_order_relaxed)) > kPlaneNear) return;
    // where this water is on the screen, gathered over the frame (this thread only)
    static float bx0 = kAreaHuge, bx1 = -kAreaHuge, by0 = kAreaHuge, by1 = -kAreaHuge;
    static uint32_t gathering = 0;
    if (frame != gathering) {
        if (gathering && bx1 >= bx0) {
            g_area[0].store(bx0, std::memory_order_relaxed), g_area[1].store(bx1, std::memory_order_relaxed);
            g_area[2].store(by0, std::memory_order_relaxed), g_area[3].store(by1, std::memory_order_relaxed);
            g_area_frame.store(gathering, std::memory_order_release);
        }
        gathering = frame, bx0 = by0 = kAreaHuge, bx1 = by1 = -kAreaHuge;
    }
    float m[12];
    for (int i = 0; i < 12; i++) m[i] = g_area_view[i].load(std::memory_order_relaxed);
    bool behind = false;
    float sum_y = 0;
    for (uint32_t i = 0; i < adjust.count; i++) {
        const uint32_t v = adjust.indices ? (uint32_t(adjust.indices[i * 2]) | uint32_t(adjust.indices[i * 2 + 1]) << 8) + s.base_vertex : adjust.start + i;
        const uint32_t p = vb + v * stride + at;
        const float x = F32(p), y = F32(p + 4), z = F32(p + 8);
        const float cz = m[2] * x + m[5] * y + m[8] * z + m[11];
        if (cz < 0.05f) {
            behind = true;
            continue;
        }
        const float tx = (m[0] * x + m[3] * y + m[6] * z + m[9]) / cz, ty = (m[1] * x + m[4] * y + m[7] * z + m[10]) / cz;
        bx0 = std::min(bx0, tx), bx1 = std::max(bx1, tx), by0 = std::min(by0, ty), by1 = std::max(by1, ty), sum_y += ty;
    }
    if (behind) {  // a part behind the eye reaches the screen's sides and its near edge
        bx0 = -kAreaHuge, bx1 = kAreaHuge;
        if (sum_y <= 0) by0 = -kAreaHuge;
        else by1 = kAreaHuge;
    }
    adjust.stage3 = texture;
}

}  // namespace pgr2
