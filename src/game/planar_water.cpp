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
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "gpu/state.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace pgr2 {
extern bool planar_water;  // src/common/game_info.cpp
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
        return;
    }
    __imp__sub_00188550(ctx, base);
    if (camera == R32(kHolder + kHolderCameras)) {
        TestCamera(ctx, camera);
        ctx.ecx = camera;
    }
}

namespace pgr2 {

// from the race view's draw (0x17FCC0, src/game/hooks.cpp), before it draws, ecx the camera
void WaterBeforeView(X86Context& ctx) {
    const uint32_t camera = ctx.ecx;
    if (g_busy || camera != R32(kHolder + kHolderCameras) || R32(camera + kCamKind) != 0 || G<uint8_t>(camera + kCamMirrored) ||
        R32(camera + kCamWidth) != 640 || R32(camera + kCamHeight) != 480) return;
    g_frame++;
    const bool wanted = planar_water && g_water_frame.load(std::memory_order_relaxed) + kFramesKept > g_frame;
    uint32_t texture = 0;
    if (wanted && Ensure(ctx)) {
        const float plane = g_water_y.load(std::memory_order_relaxed);
        // from above only: under the plane there is nothing to mirror
        if (F32(camera + kCamPosition + 4) > plane) {
            g_busy = true;
            DrawReflection(ctx, camera, plane);
            g_busy = false;
            g_plane = plane;
            static uint32_t passes = 0;
            if (++passes % 300 == 1) LOGR("planar water: pass %u, level %.3f, camera at %.1f %.1f %.1f", passes, plane, F32(camera + kCamPosition), F32(camera + kCamPosition + 4), F32(camera + kCamPosition + 8));
            texture = g_header;
        }
    }
    g_texture = texture;
    ctx.ecx = camera;
}

}  // namespace pgr2

namespace pgr2 {

// the draw filter's part: a water draw reads the reflection in place of its cube
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
    adjust.stage3 = texture;
}

}  // namespace pgr2
