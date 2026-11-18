// The light pool for the rear view mirror (mirror_light_pool, on by default; PGR2_MIRROR_POOL=0 off).
//
// At night the road's light comes from the light pool, a screen space light buffer of the main view: 0x180230 draws, from the pool
// camera (the holder's +0x10, 0x3B59D0, its source camera at +0xBEF0) with the main view's view matrix (0x188220 at the pass's
// start, the vtable's +0x18, 0x187E90, before the beams) into a 256x256 target ([0x48639C] surface, [0x4863A0] texture, [0x4863A4]
// holding the depth surface's address): the world, the cars, the street lights' glows (added, depth tested) and the cars' headlight
// beams into its alpha (0x32850: the followed car's 0x1F2A0; each other car with lamp 0 or 1 lit, 0x1F1D0, white, 25 m). The road
// reads it as material mode 8 (0x118D90), which it only is while the pool camera the road knows ([0x3CD808]) has the camera being
// drawn as its source (0x118E01, else mode 0x14, no pool); the mode's setup has 0x187B00 map the camera's box (+0x28..+0x34, 640 x
// 480 units) onto the pool's 256 texels (c167-c170) and binds [0x4863A0] (0xF6170).
//
// So the mirror's road never read a pool: no street light glows and no beams of the cars behind lit it, and the street light
// shadows the night pass 0x32D10 draws in the mirror too multiplied an unlit road and barely showed.
//
// Here, before the mirror's view (0x17FCC0 on a camera of kind 1), the pool pass runs again for the mirror: on a copy of the pool
// camera with visibility tests of its own (running the game's twice in a frame waited forever on its own tests), as the camera being
// drawn (0x3CD804), with the mirror's viewer, aspect, field of view scale, view and place, its winding turned over (the mirror's
// view is mirrored), into a pool of our own. While the mirror draws, [0x3CD808] is our camera and [0x4863A0] our pool, and the
// mirror's box maps as a full frame, as our pool was drawn. Measured on the user's recorded night race in bumper view (3200 frames,
// M4 Pro): work 5.49 -> 5.69 ms a frame, p95 7.22 -> 7.51; GPU 12.33 -> 12.42 ms; draws 4159 -> 4357.
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace pgr2 {
extern bool mirror_light_pool;  // src/common/game_info.cpp
bool g_mirror_pool_pass = false;  // our pool pass: hooks.cpp's 0x179290 lets it draw
}

X86_EXTERN_FUNC(__imp__sub_00180230);
X86_EXTERN_FUNC(__imp__sub_00187B00);
X86_EXTERN_FUNC(__imp__sub_00188220);
X86_EXTERN_FUNC(__imp__sub_00187E90);

namespace {
using kernel::G;
using kernel::R32;
using kernel::W32;

constexpr uint32_t kPoolSurface = 0x48639C, kPoolTexture = 0x4863A0;
constexpr uint32_t kPoolDepth = 0x4863A4;  // where the pool's depth surface is kept (0x4861BC), not the surface: 0x1802EA reads through it
constexpr uint32_t kNight = 0x49EF6C;
constexpr uint32_t kCamViewer = 0x10, kCamMirrored = 0x14, kCamBox = 0x28, kCamAspect = 0x38, kCamFovScale = 0x3C, kCamKind = 0x40,
                   kPoolSource = 0xBEF0;
constexpr uint32_t kPoolDraw = 0x180230;
constexpr uint32_t kCamPosition = 0xE0, kCamView = 0xF0;  // x y z; the view matrix (4x4)
constexpr uint32_t kFrustum = 0x187BD0;  // thiscall: the camera's planes from its view and projection
constexpr uint32_t kCreateSurface = 0x1C4AA0;     // stdcall (width, height, 1, format), as 0x178A55
constexpr uint32_t kSetTextureHeader = 0x1E0645;  // XGSetTextureHeader, as 0x178A74
constexpr uint32_t kResourceRelease = 0x1C31F0;   // stdcall (resource)
constexpr uint32_t kSurfaceHeader = 0x18;
constexpr uint32_t kFormatColour = 0x12, kFormatDepth = 0x30;  // LIN_A8R8G8B8, LIN_D16: the night pool's
constexpr uint32_t kSize = 256;
constexpr uint32_t kArena = 0x7F000000;  // past night_shadows.cpp's slots: the camera, then the headers and pixels
constexpr uint32_t kCameraBytes = 0xDE90;    // what 0xF6340 constructs (planar_water.cpp)
constexpr uint32_t kPoolCameraBytes = 0xBF20;  // the pool camera: the holder's +0x10 up to the cube camera at +0xBF30
constexpr uint32_t kCamTests = 0x1F0, kCamTestsEnd = 0xBD74;  // its visibility tests' slots and tests: each camera its own
constexpr uint32_t kConstruct = 0xF6340;     // thiscall: a race camera, as 0xF6AF2 builds them
constexpr uint32_t kEmptyTests = 0x187D90;   // thiscall: waits for a camera's tests and frees their slots
constexpr uint32_t kActiveCamera = 0x3CD804; // the camera being drawn (the holder 0x3B59C0 + 0x17E44)
constexpr uint32_t kPoolCamera = 0x3CD808;   // the pool camera the road reads: mode 8 only while its source is the camera drawn (0x118E01)

uint32_t g_pool = 0;                            // the game's pool camera, from its draw
uint32_t g_camera = 0;                          // our copy
uint32_t g_surface = 0, g_texture = 0, g_depth = 0, g_depth_at = 0;  // ours; g_depth_at holds g_depth
bool g_failed = false;
uint32_t g_reading = 0;       // the mirror camera whose view is drawing with our pool bound
uint32_t g_saved_texture = 0, g_saved_pool = 0;

uint32_t OwnSurface(X86Context& ctx, uint32_t format, uint32_t at, uint32_t data) {
    const uint32_t made = kernel::CallGuest(ctx, kCreateSurface, {kSize, kSize, 1, format});
    if (!made) return 0;
    std::memcpy(&G<uint8_t>(at), &G<uint8_t>(made), kSurfaceHeader);
    W32(at + 4, data);
    kernel::CallGuest(ctx, kResourceRelease, {made});
    return at;
}

bool Ensure(X86Context& ctx) {
    if (g_surface) return true;
    if (g_failed) return false;
    g_failed = true;
    const uint32_t bytes = kSize * kSize * 4, header = kArena + kCameraBytes, pixels = (header + 0x100 + 0xFFF) & ~0xFFFu;
    if (!mem::Commit(kArena, pixels + 2 * bytes - kArena)) {
        LOGR("mirror pool: no room at %08X", kArena);
        return false;
    }
    g_texture = header, g_surface = header + 0x40, g_depth = header + 0x80, g_depth_at = header + 0xC0;
    if (!OwnSurface(ctx, kFormatColour, g_surface, pixels) || !OwnSurface(ctx, kFormatDepth, g_depth, pixels + bytes)) {
        LOGR("mirror pool: no surfaces");
        g_surface = 0;
        return false;
    }
    kernel::CallGuest(ctx, kSetTextureHeader, {kSize, kSize, 1, 0, kFormatColour, 0, g_texture, pixels, 0});
    W32(g_depth_at, g_depth);
    ctx.ecx = kArena;
    kernel::CallGuest(ctx, kConstruct, {});
    g_camera = kArena;
    g_failed = false;
    LOGR("mirror pool: target %08X texture %08X depth %08X", g_surface, g_texture, g_depth);
    return true;
}

bool Wanted(uint32_t camera) {
    static const bool off = [] {
        const char* e = std::getenv("PGR2_MIRROR_POOL");
        return e && *e == '0';
    }();
    return pgr2::mirror_light_pool && !off && g_pool && R32(kNight) == 1 && R32(camera + kCamKind) == 1 && R32(g_pool + kPoolSource);
}
}  // namespace

namespace pgr2 {

// before a view's drawing (hooks.cpp, 0x17FCC0): for the mirror, its own pool
void MirrorPoolBeforeView(X86Context& ctx) {
    const uint32_t camera = ctx.ecx;
    if (g_reading || !Wanted(camera) || !Ensure(ctx) || g_pool == g_camera) return;
    const X86Context regs = ctx;
    const uint32_t pool = g_camera;
    // the game's pool camera as it is this frame, all but its tests
    std::memcpy(&G<uint8_t>(pool), &G<uint8_t>(g_pool), kCamTests);
    std::memcpy(&G<uint8_t>(pool + kCamTestsEnd), &G<uint8_t>(g_pool + kCamTestsEnd), kPoolCameraBytes - kCamTestsEnd);
    const uint32_t surface = R32(kPoolSurface), depth = R32(kPoolDepth), active = R32(kActiveCamera);
    W32(kPoolSurface, g_surface);
    W32(kPoolDepth, g_depth_at);
    W32(pool + kPoolSource, camera);
    W32(pool + kCamViewer, R32(camera + kCamViewer));
    W32(pool + kCamAspect, R32(camera + kCamAspect));
    W32(pool + kCamFovScale, R32(camera + kCamFovScale));
    G<uint8_t>(pool + kCamMirrored) = G<uint8_t>(g_pool + kCamMirrored) ^ (G<uint8_t>(camera + kCamMirrored) ? 1 : 0);
    W32(kActiveCamera, pool);
    g_mirror_pool_pass = true;
    ctx.ecx = pool;
    kernel::CallGuest(ctx, kPoolDraw, {});
    g_mirror_pool_pass = false;
    W32(kActiveCamera, active);
    W32(kPoolSurface, surface);
    W32(kPoolDepth, depth);
    ctx = regs;
    // the mirror's road reads ours
    g_saved_texture = R32(kPoolTexture);
    W32(kPoolTexture, g_texture);
    g_saved_pool = R32(kPoolCamera);
    W32(kPoolCamera, g_camera);
    g_reading = camera;
    static bool said = false;
    if (!said) LOGR("mirror pool: drawn for the mirror camera %08X (pool camera %08X)", camera, pool);
    said = true;
}

// a race's end (planar_water.cpp's hook on 0x187DC0): our camera's tests go back to the pool with the main camera's
void MirrorPoolEmptyTests(X86Context& ctx) {
    if (!g_camera) return;
    const uint32_t ecx = ctx.ecx;
    ctx.ecx = g_camera;
    kernel::CallGuest(ctx, kEmptyTests, {});
    ctx.ecx = ecx;
}

void MirrorPoolAfterView() {
    if (!g_reading) return;
    W32(kPoolTexture, g_saved_texture);
    W32(kPoolCamera, g_saved_pool);
    g_reading = 0;
}

}  // namespace pgr2

// the light pool's draw (thiscall on its camera): the game's pool camera noted
X86_FUNC(sub_00180230) {
    if (!pgr2::g_mirror_pool_pass) {
        if (g_pool != ctx.ecx) LOGR("mirror pool: the game's pool camera %08X", ctx.ecx);
        g_pool = ctx.ecx;
    }
    __imp__sub_00180230(ctx, base);
}

// our pool camera's view: the game's copies the main view's from the viewer (0x188220 at the pass's start, its vtable +0x18, 0x187E90,
// again before the beams; the pool is a screen space light buffer of the main view); ours takes the mirror's own view and place
namespace {
void MirrorView(X86Context& ctx, uint32_t camera) {
    if (!pgr2::g_mirror_pool_pass || camera != g_camera) return;
    const uint32_t source = R32(camera + kPoolSource);
    std::memcpy(&G<uint8_t>(camera + kCamView), &G<uint8_t>(source + kCamView), 64);
    std::memcpy(&G<uint8_t>(camera + kCamPosition), &G<uint8_t>(source + kCamPosition), 12);
    const uint32_t eax = ctx.eax;
    ctx.ecx = camera;
    kernel::CallGuest(ctx, kFrustum, {});
    ctx.eax = eax;
}
}  // namespace

// thiscall: the plane's point and normal; ret 0x18
X86_FUNC(sub_00188220) {
    const uint32_t camera = ctx.ecx;
    __imp__sub_00188220(ctx, base);
    MirrorView(ctx, camera);
}

// thiscall (1); ret 4
X86_FUNC(sub_00187E90) {
    const uint32_t camera = ctx.ecx;
    __imp__sub_00187E90(ctx, base);
    MirrorView(ctx, camera);
}

// the pool's place on the screen for the camera being drawn (thiscall; c167-c170): the mirror maps as a full frame while it reads
// our pool, which was drawn from its whole view
X86_FUNC(sub_00187B00) {
    const uint32_t camera = ctx.ecx;
    if (!g_reading || camera != g_reading) return __imp__sub_00187B00(ctx, base);
    uint32_t box[4];
    for (int i = 0; i < 4; i++) box[i] = R32(camera + kCamBox + i * 4);
    const uint32_t full[4] = {0, 0, 640, 480};
    for (int i = 0; i < 4; i++) W32(camera + kCamBox + i * 4, full[i]);
    __imp__sub_00187B00(ctx, base);
    for (int i = 0; i < 4; i++) W32(camera + kCamBox + i * 4, box[i]);
}

