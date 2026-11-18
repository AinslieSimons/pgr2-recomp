// Street light shadows from shadow maps. At night the race view runs 0x32D10: for each car in view it takes the road plane under
// the car, finds the lights within 50 m (the grid at 0x2D9808, records of 0x54 bytes in [0x2D97FC]), and for each one draws the
// car's shadow mesh (0xC7C00) flattened onto the plane by D3DXMatrixShadow (0x194C8C) as a multiply (blend ZERO, SRC_COLOR) of one
// shade: a hard-edged copy of the car's outline, cast as if the light were infinitely far away.
//
// street_light_shadows: 0 as the console; 1 (default) the day's shadow map from each lamp. For each light 0x32D10 keeps, the sun's
// direction (0x49EE8C, the way the light travels) and colour (0x49EEA8) are set to the lamp's, the car's depth map is drawn
// (0x1892E0) and its ground mesh (0x21920: the road's collision triangles near the car, redrawn as a multiply, cut by the map's
// comparison) shades it; then the sun is put back. The mesh's shade is base / (base + colour x -dir.y) per channel; the colour is
// chosen to give this light's shade to the power 4 (the console's flattened hull overlaps itself a few layers deep, the mesh
// multiplies once). Each light's shadow compounds with the others' (draw_filter.cpp leaves its one-shadow stencil off at night).
// street_light_reach (percent, default 200): each light's radius (+0x28, +0x2C) scaled for the pass, so more lights cast and farther;
// the ground mesh's 4 m radius grows with it (12 m at most).
//
// Batched: the pass only notes each light's shadow (car, the light's way, its colour); after it every map is drawn into a slot of
// its own, then every ground mesh with its map. A map drawn in the pass's middle sent the renderer from the frame's target to the map
// and back, a new command buffer each way (44 a frame against the console's 13 on a recorded race; 13 batched). The slots are drawn at
// a fixed 1.25 renderer pixels a guest pixel (gpu::g_fixed_scale), not the window's 3.33. A car's road triangles are gathered once a
// batch (0xBF3F0 into 0x376DE8) and drawn again for its other lights; none are drawn into the water's mirrored copy of the view
// (planar_water.cpp), where the road under the cars is not seen.
// Measured on the recorded race (tools/perf/shadow_report.py): the street light work 0.86 ms a frame, work p95 8.8 ms against the
// console's 6.9. Testing: PGR2_STREET_SHADOWS, PGR2_STREET_REACH, PGR2_SHADOW_PROFILE=1 (map, mesh and gather times in the log).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "app/perf.h"
#include "core/log.h"
#include "game/perf_zones.h"
#include "gpu/backend.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace pgr2 {
bool DrawingWaterReflection();         // src/game/planar_water.cpp
extern uint32_t street_light_shadows;  // src/common/game_info.cpp
extern uint32_t street_light_reach;    // src/common/game_info.cpp: percent of each light's radius
extern bool cpu_car_shadows;           // src/common/game_info.cpp
extern std::atomic<uint32_t> g_shadow_mark;  // src/game/car_shadows.cpp
// perf: the street lights' car shadows drawn, the cars the night pass looked at and the lights it found for them, per frame
std::atomic<uint32_t> g_night_shadows{0}, g_night_cars{0}, g_night_lights{0};
void TakeNightShadowCounts(uint32_t& shadows, uint32_t& cars, uint32_t& lights) {
    shadows = g_night_shadows.exchange(0), cars = g_night_cars.exchange(0), lights = g_night_lights.exchange(0);
}
}

X86_EXTERN_FUNC(__imp__sub_00032D10);
X86_EXTERN_FUNC(__imp__sub_00194C8C);
X86_EXTERN_FUNC(__imp__sub_0019430F);
X86_EXTERN_FUNC(__imp__sub_000C7C00);
X86_EXTERN_FUNC(__imp__sub_000BF3F0);
X86_EXTERN_FUNC(__imp__sub_0019440D);

namespace {
using kernel::R32;

constexpr uint32_t kLights = 0x2D97FC;          // the light records of the night grid, 0x54 bytes each
constexpr uint32_t kLightPlace = 0x38;           // x y z
constexpr uint32_t kLightCount = 0x2D9800;       // the records' count
constexpr uint32_t kLightRecord = 0x54, kLightRadius = 0x28, kLightInner = 0x2C, kLightType = 0x50;  // type & 0x1F: 2 and 3 cast
constexpr uint32_t kFromShadowMatrix = 0x332C6;  // 0x32D10's call of D3DXMatrixShadow returns here
constexpr uint32_t kFromMultiply = 0x3330E;      // and of D3DXMatrixMultiply (car world times shadow)
constexpr uint32_t kFromDraw = 0x33464;          // and of the shadow draw
constexpr uint32_t kShadowObject = 0x49E110;     // the sun shadow: its map drawn by 0x1892E0, applied by a car's ground mesh 0x21920
constexpr uint32_t kSunDirection = 0x49EE8C;     // +0xD7C: the way the sunlight travels (x y z), y < 0; -y scales its colour
constexpr uint32_t kSunColour = 0x49EEA8;        // ARGB
constexpr uint32_t kDrawMap = 0x1892E0;          // thiscall (car, map scale 1.0); ret 8
constexpr uint32_t kGroundMesh = 0x21920;        // thiscall on the car
constexpr uint32_t kCarAmbient = 0x1510;         // r g b a: the car's ambient, the shade's base (0x21920: base / (base + sun))
constexpr uint32_t kCarFromPointer = 0x474C;     // 0x32D10's per car pointer (ebx at the draw) is the car + this
constexpr float kDarken = 4.0f;                  // the console's shade to this power: its hull's overlapping layers
constexpr uint32_t kMapSurface = 0x486190;       // the map's depth surface, as 0x1892E0 sets it as the render target
constexpr uint32_t kMapColour = 0x486198;        // its colour surface (no data)
constexpr uint32_t kMapTexture = 0x486194;       // the map's texture, bound by 0x21920
constexpr uint32_t kMapSize = 0x4861B4;          // its width and height (1024); the viewport is this x the scale
constexpr uint32_t kCamera = 0x3CD804;           // the camera being drawn
constexpr uint32_t kCullModeCopy = 0x486146, kCullMap = 0x900;  // the game's copy of the cull mode (16 bits), the map's
constexpr uint32_t kCullMirrored = 0x901, kCamMirrored = 0x14;  // a mirrored camera's cull mode (0x186A30), its flag
constexpr uint32_t kZEnableCopy = 0x486148, kZEnableW = 2;      // its copy of ZEnable (8 bits); 2 W-buffered
constexpr uint32_t kSetCullMode = 0x1C11D0, kSetZEnable = 0x1C20F0;  // D3DDevice_SetRenderState_CullMode / _ZEnable (stdcall, value)
constexpr uint32_t kCameraSetup = 0x185640;      // thiscall on the camera: its transforms for the draws that follow
constexpr uint32_t kSetSurfaceHeader = 0x1E0604; // XGSetSurfaceHeader (width, height, format, header, data, pitch), as 0x178AF5
constexpr uint32_t kSetTextureHeader = 0x1E0645; // XGSetTextureHeader (w, h, levels, usage, format, pool, header, data, pitch)
constexpr uint32_t kSetVSConstant = 0x1BFEF0;    // fastcall (ecx register, edx data; count), as 0x189C51
constexpr uint32_t kTextureMatrixRegister = 0x8F;  // the map's texture matrix, 4 registers (0x189C4C)
constexpr uint32_t kFromTranspose = 0x189C43;    // 0x1892E0's call of D3DXMatrixTranspose returns here
constexpr uint32_t kFormatDepth = 0x30, kFormatColour = 0x11;  // LIN_D16 (as the map's texture), the map's colour surface
constexpr uint32_t kBatchArena = 0x78000000, kBatchArenaBytes = 0x06000000;  // the maps' slots, past planar_water.cpp's memory
constexpr float kBatchScale = 1.25f;             // renderer pixels a slot's pixel (up to 1400 across keeps its clear in the batch)
constexpr uint32_t kRoadList = 0x376DE8;         // 0xBF3F0's own list of gathered road triangles

using Matrix = float[16];  // D3DX layout: row vectors, rows of 4

bool g_pass = false;        // inside 0x32D10
bool g_inner = false;       // our own calls of the ground mesh
bool g_reflection = false;  // the pass is drawing into the water's reflection
bool g_have = false;        // the light's place caught for the draw that follows
uint32_t last_car = 0;      // perf: the car of the last light, for the count of cars
float g_lamp[3] = {0};      // the light's place
bool g_capture = false, g_captured = false;
Matrix g_captured_m;        // the texture matrix of the map last drawn
uint32_t g_upload = 0;      // guest memory for the uploaded matrix
uint32_t g_mesh_car = 0, g_gathered_car = 0;  // the car whose mesh is drawing; the car whose road triangles are in kRoadList

float F(uint32_t va) {
    const uint32_t v = R32(va);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
void WF(uint32_t va, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    kernel::W32(va, v);
}

// a map's slot: its surfaces' and texture's headers and its pixels
struct Slot {
    uint32_t surface = 0, colour = 0, texture = 0, data = 0;
};

// a slot's headers and pixels, made the first time it is used. The colour surface is on the depth's memory (the map writes no
// colour, so the renderer draws depth alone): a colour surface with no memory would be made at the window's scale, and the renderer
// leaves out a depth target of another size than the colour's
bool PrepareSlot(X86Context& ctx, Slot& t, uint32_t at, uint32_t bytes, uint32_t s) {
    if (t.surface) return true;
    if (!mem::Commit(at, bytes)) return false;
    t.surface = at, t.colour = at + 0x40, t.texture = at + 0x80, t.data = at + 0x1000;
    std::memset(&kernel::G<uint8_t>(t.data), 0xFF, s * s * 2);  // the far plane: a lost target casts nothing
    kernel::CallGuest(ctx, kSetSurfaceHeader, {s, s, kFormatDepth, t.surface, t.data, s * 2});
    kernel::CallGuest(ctx, kSetSurfaceHeader, {s, s, kFormatColour, t.colour, t.data, 0});
    kernel::CallGuest(ctx, kSetTextureHeader, {s, s, 1, 0, kFormatDepth, 0, t.texture, t.data, s * 2});
    return true;
}

// the car's depth map with the light travelling along dir (world), drawn by 0x1892E0 into a slot of size x size: true when its
// texture matrix (view^-1 x light view x projection x bias) was caught, in g_captured_m. 0x1892E0 draws a viewport of the map's
// size x the scale and widens its view by 1 / scale (0x1898EC): the map's size is made the slot's for the draw, at scale 1
bool DrawMap(X86Context& ctx, const Slot& t, uint32_t size, uint32_t car, const float dir[3]) {
    uint8_t sun[12];
    std::memcpy(sun, &kernel::G<uint8_t>(kSunDirection), 12);
    std::memcpy(&kernel::G<uint8_t>(kSunDirection), dir, 12);
    const uint32_t surface = R32(kMapSurface), colour = R32(kMapColour), map_size = R32(kMapSize);
    kernel::W32(kMapSurface, t.surface);
    kernel::W32(kMapColour, t.colour);
    kernel::W32(kMapSize, size);
    g_capture = true, g_captured = false;
    ctx.ecx = kShadowObject;
    kernel::CallGuest(ctx, kDrawMap, {car, 0x3F800000u});
    g_capture = false;
    kernel::W32(kMapSize, map_size);
    kernel::W32(kMapSurface, surface);
    kernel::W32(kMapColour, colour);
    std::memcpy(&kernel::G<uint8_t>(kSunDirection), sun, 12);
    return g_captured;
}

// the car's ground mesh (0x21920) reading a map of ours: its texture matrix (not transposed) at c[0x8F], its texture in the map's place
void DrawMesh(X86Context& ctx, uint32_t car, uint32_t texture_header, const Matrix matrix) {
    // the states 0x1892E0 leaves, which the ground mesh draws with (the night pass's own differ): culling 0x900 (set for the car's
    // depth at 0x189A3D, never put back) and Z as W (0x189ABE); each through the game's copy of the state, as it sets them
    // A mirrored camera (the rear view mirror, +0x14) turns the winding over: the game culls with 0x901 there (0x186A30), and the
    // mesh culled with 0x900 drew nothing in the mirror
    const uint16_t cull = kernel::G<uint8_t>(R32(kCamera) + kCamMirrored) ? kCullMirrored : kCullMap;
    if (kernel::R16(kCullModeCopy) != cull) {
        kernel::W16(kCullModeCopy, cull);
        kernel::CallGuest(ctx, kSetCullMode, {cull});
    }
    if (kernel::G<uint8_t>(kZEnableCopy) != kZEnableW) {
        kernel::G<uint8_t>(kZEnableCopy) = kZEnableW;
        kernel::CallGuest(ctx, kSetZEnable, {kZEnableW});
    }
    // the camera's own transforms again, as 0x1892E0 ends with (0x189ADC)
    ctx.ecx = R32(kCamera);
    kernel::CallGuest(ctx, kCameraSetup, {});
    Matrix m;  // transposed, as the shader reads it
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) m[r * 4 + c] = matrix[c * 4 + r];
    std::memcpy(&kernel::G<uint8_t>(g_upload), m, sizeof(Matrix));
    ctx.ecx = kTextureMatrixRegister;
    ctx.edx = g_upload;
    kernel::CallGuest(ctx, kSetVSConstant, {16});
    const uint32_t texture = R32(kMapTexture);
    kernel::W32(kMapTexture, texture_header);
    ctx.ecx = car;
    kernel::CallGuest(ctx, kGroundMesh, {});
    kernel::W32(kMapTexture, texture);
}

// the shadows noted in the pass, drawn after it: slots of the map's size (1024) in kBatchArena, 23 of them; a frame with more
// lights runs more batches
struct Pending {
    uint32_t car, colour;
    float dir[3];
};
std::vector<Pending> g_pending;
std::vector<Slot> g_batch;
uint32_t g_batch_size = 0, g_batch_bytes = 0;
bool g_batch_failed = false;
Matrix g_batch_matrix[64];

bool EnsureBatch() {
    if (!g_batch.empty()) return true;
    if (g_batch_failed) return false;
    g_batch_failed = true;
    g_batch_size = R32(kMapSize);
    if (!g_batch_size) return false;
    // the renderer takes any depth target as 4 bytes a pixel (metal.mm EnsureTarget) and reads that much of it: room for it
    g_batch_bytes = (0x1000 + g_batch_size * g_batch_size * 4 + 0xFFF) & ~0xFFFu;
    const uint32_t slots = std::min<uint32_t>(kBatchArenaBytes / g_batch_bytes, 64);
    if (!slots) return false;
    g_batch.resize(slots);
    g_upload = mem::AllocSystem(sizeof(Matrix), 16);
    g_batch_failed = false;
    gpu::g_fixed_scale = kBatchScale;
    gpu::g_fixed_scale_lo = kBatchArena;
    gpu::g_fixed_scale_hi = kBatchArena + kBatchArenaBytes;
    LOGR("street light shadows: batches of %u maps of %ux%u, drawn at %.2f", slots, g_batch_size, g_batch_size, kBatchScale);
    return true;
}

// testing: PGR2_SHADOW_PROFILE=1 logs the time a map, a mesh and its gather every 600 passes
struct Profile {
    double map_ms = 0, mesh_ms = 0, gather_ms = 0;
    uint64_t maps = 0, meshes = 0, passes = 0;
};
Profile g_profile;
const bool g_profiling = std::getenv("PGR2_SHADOW_PROFILE") != nullptr;
double NowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void FlushBatch(X86Context& ctx) {
    if (g_profiling && ++g_profile.passes % 600 == 0 && g_profile.maps) {
        LOGR("shadow profile: %.1f maps a pass, %.3f ms a map, %.3f ms a mesh (%.3f gathering); a pass %.3f ms", double(g_profile.maps) / 600,
             g_profile.map_ms / g_profile.maps, g_profile.mesh_ms / std::max<uint64_t>(1, g_profile.meshes),
             g_profile.gather_ms / std::max<uint64_t>(1, g_profile.meshes), (g_profile.map_ms + g_profile.mesh_ms) / 600);
        g_profile = Profile{0, 0, 0, 0, 0, g_profile.passes};
    }
    if (g_pending.empty()) return;
    if (!EnsureBatch()) {
        g_pending.clear();
        return;
    }
    perf::Zone zone(pgr2::kZStreetLight);
    uint8_t sun[12];
    std::memcpy(sun, &kernel::G<uint8_t>(kSunDirection), 12);
    const uint32_t sun_colour = R32(kSunColour);
    const bool cpu = pgr2::cpu_car_shadows;
    pgr2::cpu_car_shadows = false;  // the mesh of each car alone (car_shadows.cpp would add the others')
    g_inner = true;
    static bool ok[64];
    for (size_t start = 0; start < g_pending.size(); start += g_batch.size()) {
        const size_t end = std::min(g_pending.size(), start + g_batch.size());
        for (size_t i = start; i < end; i++) {
            Slot& t = g_batch[i - start];
            const Pending& p = g_pending[i];
            const double t0 = g_profiling ? NowMs() : 0;
            ok[i - start] = PrepareSlot(ctx, t, kBatchArena + uint32_t(i - start) * g_batch_bytes, g_batch_bytes, g_batch_size) &&
                            DrawMap(ctx, t, g_batch_size, p.car, p.dir);
            if (g_profiling) g_profile.map_ms += NowMs() - t0, g_profile.maps++;
            if (ok[i - start]) std::memcpy(g_batch_matrix[i - start], g_captured_m, sizeof(Matrix));
        }
        g_gathered_car = 0;  // the maps' draws may have used the list
        for (size_t i = start; i < end; i++) {
            if (!ok[i - start]) continue;
            const Pending& p = g_pending[i];
            std::memcpy(&kernel::G<uint8_t>(kSunDirection), p.dir, 12);  // the mesh's shade: base / (base + colour x -dir.y)
            kernel::W32(kSunColour, p.colour);
            pgr2::g_shadow_mark = pgr2::g_shadow_mark % 255 + 1;  // each light's shadow its own (draw_filter: one shadow a mark)
            const double t0 = g_profiling ? NowMs() : 0;
            g_mesh_car = p.car;
            DrawMesh(ctx, p.car, g_batch[i - start].texture, g_batch_matrix[i - start]);
            if (g_profiling) g_profile.mesh_ms += NowMs() - t0, g_profile.meshes++;
        }
    }
    g_mesh_car = g_gathered_car = 0;
    g_inner = false;
    pgr2::cpu_car_shadows = cpu;
    std::memcpy(&kernel::G<uint8_t>(kSunDirection), sun, 12);
    kernel::W32(kSunColour, sun_colour);
    g_pending.clear();
}

}  // namespace

// the night car shadows (thiscall on the car list)
X86_FUNC(sub_00032D10) {
    // the water's mirrored copy of the view (src/game/planar_water.cpp) draws the console's shadows alone: the road under the cars
    // is not in it, and the maps and meshes cost as much again as the view's own
    g_reflection = pgr2::DrawingWaterReflection();
    g_pass = true;
    g_have = false;
    last_car = 0;
    // reach: each light's radius (and the inner one where it is at full strength) widened for this pass only, so lights a little
    // farther off still cast, and stronger: 0x32D10 keeps a light within its radius (+0x28) of the car, at a strength (0x409E0)
    // falling from the inner radius (+0x2C) to the outer
    const uint32_t reach = pgr2::street_light_shadows && !g_reflection ? std::clamp<uint32_t>(pgr2::street_light_reach, 50, 400) : 100;
    const uint32_t records = R32(kLights), count = std::min<uint32_t>(R32(kLightCount), 65536);
    static std::vector<float> saved;
    saved.clear();
    if (reach != 100 && records)
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t r = records + i * kLightRecord, type = R32(r + kLightType) & 0x1F;
            if (type != 2 && type != 3) continue;
            saved.push_back(F(r + kLightRadius));
            saved.push_back(F(r + kLightInner));
            WF(r + kLightRadius, F(r + kLightRadius) * float(reach) / 100.0f);
            WF(r + kLightInner, F(r + kLightInner) * float(reach) / 100.0f);
        }
    {
        perf::Zone zone(pgr2::kZNightPass);
        __imp__sub_00032D10(ctx, base);
        FlushBatch(ctx);
    }
    if (!saved.empty())
        for (uint32_t i = 0, k = 0; i < count && k + 1 < saved.size(); i++) {
            const uint32_t r = records + i * kLightRecord, type = R32(r + kLightType) & 0x1F;
            if (type != 2 && type != 3) continue;
            WF(r + kLightRadius, saved[k++]);
            WF(r + kLightInner, saved[k++]);
        }
    g_pass = false;
}

// D3DXMatrixShadow (out, light, plane; stdcall): in the night pass, the light's place taken from the record being looked at (esi,
// its offset in the array)
X86_FUNC(sub_00194C8C) {
    if (g_pass && R32(ctx.esp) == kFromShadowMatrix) {
        const uint32_t record = R32(kLights) + ctx.esi;
        for (int i = 0; i < 3; i++) g_lamp[i] = F(record + kLightPlace + i * 4);
    }
    __imp__sub_00194C8C(ctx, base);
}

// D3DXMatrixMultiply (out, a, b; stdcall): the car's world times the light's shadow, just before the light's draw
X86_FUNC(sub_0019430F) {
    if (g_pass && R32(ctx.esp) == kFromMultiply) g_have = true;
    __imp__sub_0019430F(ctx, base);
}

// the car's shadow mesh drawn (thiscall on the mesh: shade; ret 4): noted for the batch in its place
X86_FUNC(sub_000C7C00) {
    if (g_inner || !g_pass || R32(ctx.esp) != kFromDraw) {
        __imp__sub_000C7C00(ctx, base);
        return;
    }
    const uint32_t car = ctx.ebx - kCarFromPointer;
    if (perf::On()) {
        pgr2::g_night_shadows.fetch_add(1, std::memory_order_relaxed);
        pgr2::g_night_lights.fetch_add(1, std::memory_order_relaxed);
        if (car != last_car) pgr2::g_night_cars.fetch_add(1, std::memory_order_relaxed);
    }
    last_car = car;
    if (!g_have || g_reflection || !pgr2::street_light_shadows) {
        __imp__sub_000C7C00(ctx, base);
        return;
    }
    g_have = false;
    perf::Zone light_zone(pgr2::kZStreetLight);
    // the day's machinery, pointed at the lamp: the colour is chosen so that the ground mesh's shade equals the night pass's for this
    // light, to the power kDarken
    const uint32_t shade = R32(ctx.esp + 4);
    Pending p;
    p.car = car;
    p.dir[0] = F(car + 0x44) - g_lamp[0], p.dir[1] = F(car + 0x48) - g_lamp[1], p.dir[2] = F(car + 0x4C) - g_lamp[2];
    const float dl = std::sqrt(p.dir[0] * p.dir[0] + p.dir[1] * p.dir[1] + p.dir[2] * p.dir[2]);
    for (float& c : p.dir) c = dl > 0 ? c / dl : 0;
    const float lift = std::max(-p.dir[1], 0.05f);  // the sun's colour is scaled by this
    p.colour = 0;
    for (int c = 0; c < 4; c++) {  // channels as 0x21920 pairs them: ambient +0, +4, +8, +C with colour bytes 16, 8, 0, 24
        const int shift = c == 0 ? 16 : c == 1 ? 8 : c == 2 ? 0 : 24;
        const float want = std::clamp(std::pow(float(shade >> shift & 0xFF) / 255.0f, kDarken), 0.02f, 1.0f);
        const float ambient = F(car + kCarAmbient + c * 4);
        const float light = ambient * (1.0f / want - 1.0f) / lift;  // colour x lift = ambient (1 / shade - 1)
        p.colour |= uint32_t(std::clamp(std::lround(light * 255.0f), 0L, 255L)) << shift;
    }
    g_pending.push_back(p);
    ctx.esp += 8;  // ret 4
}

// the ground mesh's road triangles (stdcall on 0x381910: list, centre, radius, 4; ret 0x10): with no list it gathers into its own
// (0x376DE8), then draws the list. For a street light's shadow the radius grows with the reach, as shadows from farther lamps are
// longer than the day's 4 m; a car's other shadows in the batch draw the list its first gathered
X86_FUNC(sub_000BF3F0) {
    if (!g_inner) {
        __imp__sub_000BF3F0(ctx, base);
        return;
    }
    WF(ctx.esp + 12, std::min(F(ctx.esp + 12) * float(std::clamp<uint32_t>(pgr2::street_light_reach, 100, 300)) / 100.0f, 12.0f));
    if (R32(ctx.esp + 4) == 0 && g_mesh_car && g_mesh_car == g_gathered_car) kernel::W32(ctx.esp + 4, kRoadList);
    const bool gathers = R32(ctx.esp + 4) == 0;
    const double t0 = g_profiling ? NowMs() : 0;
    __imp__sub_000BF3F0(ctx, base);
    if (g_profiling) g_profile.gather_ms += NowMs() - t0;
    if (gathers) g_gathered_car = g_mesh_car;
}

// D3DXMatrixTranspose (out, in; stdcall): in our map's draw the texture matrix 0x1892E0 is about to upload is kept
X86_FUNC(sub_0019440D) {
    if (g_capture && R32(ctx.esp) == kFromTranspose) {
        std::memcpy(g_captured_m, &kernel::G<uint8_t>(R32(ctx.esp + 8)), sizeof(Matrix));
        g_captured = true;
    }
    __imp__sub_0019440D(ctx, base);
}
