// Street-light shadows, softened and cast from the lamp. At night the race view runs 0x32D10: for each car in view it takes the
// road plane under the car, finds the lights within 50 m (the grid at 0x2D9808, records of 0x54 bytes in [0x2D97FC]), and for each
// one draws the car's shadow mesh (0xC7C00) flattened onto the plane by D3DXMatrixShadow (0x194C8C) as a multiply (blend ZERO,
// SRC_COLOR) of one shade. The game gives D3DXMatrixShadow the direction to the light with w 0, a light infinitely far away: the
// shadow is a hard-edged copy of the car's outline, the same length whether the lamp is beside the car or far down the road.
//
// street_light_shadows: 0 as the console; 1 soft: each shadow drawn kSamples times from points spread over the lamp (a disc of
// kLampRadius across the road plane), each pass's vertex colours raised to the power 1 / kSamples, so where every sample's shadow
// falls the road is as dark as before and the edge fades out; 2 soft and from the lamp: the same with w 1, the lamp a point, so a
// shadow lengthens as the car leaves the lamp and points away from it.
// 3 (default) the day's shadow map from each lamp: for each light 0x32D10 keeps, the sun's direction (0x49EE8C, the way the light
// travels) and colour (0x49EEA8) are set to the lamp's, the car's depth map is drawn (0x1892E0) and its ground mesh (0x21920: the
// road's collision triangles near the car, redrawn as a multiply, cut by the map's comparison) shades it; then the sun is put back.
// The mesh's shade is base / (base + colour x -dir.y) per channel; the colour is chosen to give this light's shade to the power
// PGR2_MAP_DARKEN (4: the console's flattened hull overlaps itself a few layers deep, the mesh multiplies once). Each light's
// shadow compounds with the others' (draw_filter.cpp leaves its one-shadow stencil off at night).
// street_light_reach (percent, default 200): each light's radius (+0x28, +0x2C) scaled for the pass, so more lights cast and farther;
// the ground mesh's 4 m radius grows with it (12 m at most). About 3x the shadows of the game's own reach at 200 (a recorded race).
// Cost: one depth map and mesh per car per light, 300 to 800 a second in a race: a frame rate dip. Mode 3 also leaves the last
// lamp's map where the game's car shading reads the sun's (the followed car's windows go dark).
// 4 as 3 with each car's maps kept, one per direction from the car (below): about 6 map draws a frame instead of 660, the
// street-light pass 1.5 ms instead of 2.0 (a recorded race, tools/perf/shadow_report.py). The views either side of a lamp's way
// are cross-faded: a lamp's shadow switched view about 4 times a second, and its outline and length jumped (the abrupt changes
// against mode 3 halved, frame by frame on a recorded stretch; mode 3 and 4 on alternate frames, PGR2_TILE_ALTERNATE, compares
// them at the same moments in one run).
// Each matrix is built as the XDK's D3DXMatrixShadow builds it (its sign is the opposite of the D3DX documentation's) and scaled to
// end with the w the game's own ends with (0x30AE0 divides the game's by d): the draw is W-buffered, so w is its depth, and a
// different w put the shadow behind the road or behind the camera. With one sample, no radius and mode 1 the matrix equals the
// game's to 4 decimals (checked). For testing: PGR2_STREET_SHADOWS, PGR2_STREET_REACH, PGR2_SHADOW_SAMPLES, PGR2_LAMP_RADIUS,
// PGR2_MAP_DARKEN, PGR2_ONLY_LIGHT (one light's shadow per car), PGR2_NO_NIGHT_CARS, PGR2_HL_LOG.
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
extern uint32_t street_light_shadows;  // src/common/game_info.cpp
extern uint32_t street_light_reach;    // src/common/game_info.cpp: percent of each light's radius
extern bool cpu_car_shadows;           // src/common/game_info.cpp
extern std::atomic<uint32_t> g_shadow_mark;  // src/game/car_shadows.cpp
float g_shade_power = 0;               // while above 0, each vertex colour set is raised to it (src/game/headlight_shadows.cpp)
// perf: the street lights' car shadows drawn, the cars the night pass looked at and the lights it found for them, per frame
std::atomic<uint32_t> g_night_shadows{0}, g_night_cars{0}, g_night_lights{0};
void TakeNightShadowCounts(uint32_t& shadows, uint32_t& cars, uint32_t& lights) {
    shadows = g_night_shadows.exchange(0), cars = g_night_cars.exchange(0), lights = g_night_lights.exchange(0);
}
uint32_t ShadePower(uint32_t colour) {
    if (g_shade_power <= 0) return colour;
    uint32_t out = colour & 0xFF000000u;
    for (int s = 0; s < 24; s += 8)
        out |= uint32_t(std::lround(std::pow(float(colour >> s & 0xFF) / 255.0f, g_shade_power) * 255.0f)) << s;
    return out;
}
}

X86_EXTERN_FUNC(__imp__sub_00032D10);
X86_EXTERN_FUNC(__imp__sub_00194C8C);
X86_EXTERN_FUNC(__imp__sub_0019430F);
X86_EXTERN_FUNC(__imp__sub_00030AE0);
X86_EXTERN_FUNC(__imp__sub_000C7C00);
X86_EXTERN_FUNC(__imp__sub_000BF3F0);
X86_EXTERN_FUNC(__imp__sub_00022470);
X86_EXTERN_FUNC(__imp__sub_0019440D);

namespace {
using kernel::R32;

constexpr uint32_t kLights = 0x2D97FC;          // the light records of the night grid, 0x54 bytes each
constexpr uint32_t kLightPlace = 0x38;           // x y z
constexpr uint32_t kLightCount = 0x2D9800;       // the records' count
constexpr uint32_t kLightRecord = 0x54, kLightRadius = 0x28, kLightInner = 0x2C, kLightType = 0x50;  // type & 0x1F: 2 and 3 cast
constexpr uint32_t kFromShadowMatrix = 0x332C6;  // 0x32D10's call of D3DXMatrixShadow returns here
constexpr uint32_t kFromMultiply = 0x3330E;      // and of D3DXMatrixMultiply (car world times shadow)
constexpr uint32_t kFromScale = 0x332DA;         // and of 0x30AE0, which divides the shadow matrix by a number
constexpr uint32_t kFromDraw = 0x33464;          // and of the shadow draw
constexpr uint32_t kLoadWorld = 0x1854F0;        // cdecl (matrix): the world matrix for the next draws
constexpr uint32_t kDraw = 0xC7C00;              // thiscall (shade); ret 4
constexpr int kSamplesDefault = 9;               // the lamp's centre and 8 around it
constexpr float kLampRadiusDefault = 0.6f;       // metres: a street lamp's head
constexpr uint32_t kShadowObject = 0x49E110;     // the sun shadow: its map drawn by 0x1892E0, applied by a car's ground mesh 0x21920
constexpr uint32_t kSunDirection = 0x49EE8C;     // +0xD7C: the way the sunlight travels (x y z), y < 0; -y scales its colour
constexpr uint32_t kSunColour = 0x49EEA8;        // ARGB
constexpr uint32_t kDrawMap = 0x1892E0;          // thiscall (car, map scale 1.0); ret 8
constexpr uint32_t kGroundMesh = 0x21920;        // thiscall on the car
constexpr uint32_t kCarAmbient = 0x1510;         // r g b a: the car's ambient, the shade's base (0x21920: base / (base + sun))
constexpr uint32_t kCarFromPointer = 0x474C;     // 0x32D10's per car pointer (ebx at the draw) is the car + this
constexpr uint32_t kFirstCar = 0x298F90;          // the race's first car (0x297390 + 0x1C00), testing
constexpr uint32_t kCarRoof = 0xB0;             // the car's height (metres)
constexpr float kMinHeight = 3.0f;               // a lamp nearer the road plane than this is treated as this high (no endless shadows)

using Matrix = float[16];  // D3DX layout: row vectors, rows of 4

bool g_pass = false;     // inside 0x32D10
bool g_inner = false;    // our own calls of the draw
uint32_t last_car = 0, light_index = 0;  // testing: which of a car's lights this is, in the pass
bool g_in_map = false;   // our call of the map draw (0x1892E0) is running
float g_fit_radius = 0;  // the ground mesh's radius for this light's shadow (PGR2_MESH_FIT), 0 the reach's
uint64_t g_map_ticks = 0, g_mesh_ticks = 0;  // testing: time in our map and mesh calls (PGR2_HL_LOG)
float g_plane[4] = {0};  // the road plane as the game gave it (normalised here)
float g_lamp[3] = {0};   // the light's place
float g_dir[4] = {0};    // the direction the game passed (w 0)
float g_divide = 1;      // what 0x30AE0 divided the game's shadow matrix by
Matrix g_world;          // the car's world matrix
bool g_have = false;     // all three caught for the draw that follows
uint32_t g_scratch = 0;  // guest memory for a matrix

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

// D3DXMatrixShadow as the XDK's (0x194C8C; checked against its result): the plane normalised, d = P . L,
// M[i][j] = d [i == j] - P[i] L[j] (row vectors)
void ShadowMatrix(const float plane[4], const float light[4], Matrix m) {
    const float n = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
    float p[4];
    for (int i = 0; i < 4; i++) p[i] = n > 0 ? plane[i] / n : plane[i];
    const float d = p[0] * light[0] + p[1] * light[1] + p[2] * light[2] + p[3] * light[3];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) m[i * 4 + j] = (i == j ? d : 0.0f) - p[i] * light[j];
}

void Multiply(const Matrix a, const Matrix b, Matrix out) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = v;
        }
}


// ---- mode 4: each car's depth maps kept, one per direction from the car, drawn once and used by every light near that direction.
// A map is drawn by the game's own 0x1892E0 with the sun pointed along the view, into a tile of our own (a D16 surface and texture
// in kTileArena), its scale chosen so that the viewport is the tile. Its end builds the texture matrix the ground mesh reads at
// c[0x8F]: M = view^-1 x light view x projection x bias, transposed by 0x19440D, where the hook keeps it. Kept relative to the car,
// K = car x view x M (the car's world and the camera's view as the map was drawn), a later frame's matrix is
// view_now^-1 x car_now^-1 x K, uploaded before the ground mesh (0x21920) is drawn with the tile's texture bound in the map's place.
// Views: 16 azimuths x 10 elevations + straight down; those either side of the lamp's way cross-faded (ChooseViews), each sheared
// to the lamp's own direction (ShearToward). A tile
// is drawn when first wanted (at most 4 a frame, PGR2_TILE_BUDGET; until then the car's nearest tile within two views stands in) and again after
// 20 s; the slots (about 750 of 256x256, the car filling each) go least recently used first. The ground mesh draws with the states 0x1892E0 leaves (cull
// 0x900, Z as W) and the camera's transforms, else nothing shows; its stage 0 filtered, for 2x2 comparisons (softer edges).
constexpr uint32_t kMapSurface = 0x486190;       // the map's depth surface, as 0x1892E0 sets it as the render target
constexpr uint32_t kMapColour = 0x486198;        // its colour surface (no data)
constexpr uint32_t kMapTexture = 0x486194;       // the map's texture, bound by 0x21920
constexpr uint32_t kMapSize = 0x4861B4;          // its width and height (1024); the viewport is this x the scale
constexpr uint32_t kCamera = 0x3CD804;           // the camera being drawn; its view at +0xF0
constexpr uint32_t kCameraView = 0xF0;
constexpr uint32_t kCullModeCopy = 0x486146, kCullMap = 0x900;  // the game's copy of the cull mode (16 bits), the map's
constexpr uint32_t kZEnableCopy = 0x486148, kZEnableW = 2;      // its copy of ZEnable (8 bits); 2 W-buffered
constexpr uint32_t kSetCullMode = 0x1C11D0, kSetZEnable = 0x1C20F0;  // D3DDevice_SetRenderState_CullMode / _ZEnable (stdcall, value)
constexpr uint32_t kStage0 = 0x1D0540;         // D3D's deferred texture states, stage 0 (32 a stage; config/game.toml)
constexpr uint32_t kTssMag = 3, kTssMin = 4, kTexfLinear = 2;
constexpr uint32_t kDirtyFlags = 0x1D0538, kDirtyStage0 = 1;  // stage 0's states sent with the next draw (as 0x20580 marks them)
constexpr uint32_t kCameraSetup = 0x185640;      // thiscall on the camera: its transforms for the draws that follow
constexpr uint32_t kSetSurfaceHeader = 0x1E0604; // XGSetSurfaceHeader (width, height, format, header, data, pitch), as 0x178AF5
constexpr uint32_t kSetTextureHeader = 0x1E0645; // XGSetTextureHeader (w, h, levels, usage, format, pool, header, data, pitch)
constexpr uint32_t kSetVSConstant = 0x1BFEF0;    // fastcall (ecx register, edx data; count), as 0x189C51
constexpr uint32_t kTextureMatrixRegister = 0x8F;  // the map's texture matrix, 4 registers (0x189C4C)
constexpr uint32_t kFromTranspose = 0x189C43;    // 0x1892E0's call of D3DXMatrixTranspose returns here
constexpr uint32_t kCarWorld = 0x18;             // the car's vtable: thiscall, its world matrix
constexpr uint32_t kFormatDepth = 0x30, kFormatColour = 0x11;  // LIN_D16 (as the map's texture), the map's colour surface
constexpr uint32_t kTileArena = 0x72000000, kTileArenaBytes = 0x06000000;  // past planar_water.cpp's
constexpr uint32_t kBatchArena = 0x78000000, kBatchArenaBytes = 0x06000000;  // mode 3's maps, drawn together (FlushBatch)
constexpr int kAzimuths = 16;
// degrees below the car's level, the way the light travels: close where a shadow's length (height / tan) changes fastest
constexpr float kElevations[] = {4, 7, 10, 14, 19, 25, 32, 41, 52, 66};
constexpr int kElevationCount = int(sizeof(kElevations) / sizeof(kElevations[0]));
constexpr int kViews = kAzimuths * kElevationCount + 1;  // and straight down

struct Tile {
    uint32_t car = 0;
    int view = -1;
    uint32_t used = 0, made = 0;  // frames
    uint32_t surface = 0, colour = 0, texture = 0, data = 0;
    bool ready = false;
    Matrix k;
};
std::vector<Tile> g_tiles;
uint32_t g_tile_size = 0, g_tile_bytes = 0, g_frame = 0, g_made_this_frame = 0;
bool g_tiles_failed = false, g_capture = false, g_captured = false;
Matrix g_captured_m;
uint32_t g_tiles_made = 0, g_tiles_reused = 0, g_tiles_missing = 0, g_tiles_faded = 0;  // testing (PGR2_HL_LOG)
uint32_t g_upload = 0;  // guest memory for the uploaded matrix

// the view's direction in the car's space: the way the light travels, y down
void ViewDirection(int view, float out[3]) {
    if (view == kViews - 1) {
        out[0] = 0, out[1] = -1, out[2] = 0;
        return;
    }
    const float el = kElevations[view / kAzimuths] * 3.14159265f / 180.0f, az = float(view % kAzimuths) * 6.2831853f / kAzimuths;
    out[0] = std::cos(el) * std::cos(az), out[1] = -std::sin(el), out[2] = std::cos(el) * std::sin(az);
}

int NearestView(const float dir[3]) {
    int best = 0;
    float best_dot = -2;
    for (int v = 0; v < kViews; v++) {
        float d[3];
        ViewDirection(v, d);
        const float dot = d[0] * dir[0] + d[1] * dir[1] + d[2] * dir[2];
        if (dot > best_dot) best_dot = dot, best = v;
    }
    return best;
}

// the views for a light's way (in the car's space) and each one's share of the shadow: the elevation bands and azimuths either side
// of it, cross-faded over the middle kFade of the step between them (PGR2_TILE_FADE, 0 the nearest alone), so that a lamp's shadow
// passes from one view's outline to the next rather than switching. Up to 4 near a corner of the grid, 1 most of the time.
struct Pick {
    int view;
    float share;
};
int ChooseViews(const float dir[3], Pick out[4]) {
    static const float fade = std::getenv("PGR2_TILE_FADE") ? float(std::atof(std::getenv("PGR2_TILE_FADE"))) : 0.15f;
    auto blend = [](float t) {  // the far side's share at t (0 .. 1) across a step
        if (fade <= 0) return t < 0.5f ? 0.0f : 1.0f;
        return std::clamp((t - 0.5f) / fade + 0.5f, 0.0f, 1.0f);
    };
    const float horizontal = std::sqrt(dir[0] * dir[0] + dir[2] * dir[2]);
    const float el = std::atan2(-dir[1], horizontal) * 57.29578f;
    // the bands and straight down as one more at 90 degrees
    auto band_at = [](int b) { return b < kElevationCount ? kElevations[b] : 90.0f; };
    int band = 0;
    while (band + 1 <= kElevationCount && band_at(band + 1) <= el) band++;
    const float upper = band < kElevationCount ? blend((std::max(el, band_at(0)) - band_at(band)) / (band_at(band + 1) - band_at(band))) : 0.0f;
    float az = std::atan2(dir[2], dir[0]) * 57.29578f;
    if (az < 0) az += 360.0f;
    const float at = az * kAzimuths / 360.0f, frac = at - std::floor(at), next = blend(frac);
    const int a0 = int(std::floor(at)) % kAzimuths, a1 = (a0 + 1) % kAzimuths;
    auto view_of = [](int b, int a) { return b >= kElevationCount ? kViews - 1 : b * kAzimuths + a; };
    const Pick all[4] = {{view_of(band, a0), (1 - upper) * (1 - next)}, {view_of(band, a1), (1 - upper) * next},
                         {view_of(band + 1, a0), upper * (1 - next)}, {view_of(band + 1, a1), upper * next}};
    int n = 0;
    float total = 0;
    for (const Pick& p : all) {
        if (p.share < 0.03f) continue;
        bool merged = false;
        for (int k = 0; k < n; k++)
            if (out[k].view == p.view) out[k].share += p.share, merged = true;
        if (!merged) out[n++] = p;
        total += p.share;
    }
    for (int k = 0; k < n; k++) out[k].share /= total;
    std::sort(out, out + n, [](const Pick& a, const Pick& b) { return a.share > b.share; });
    return n;
}

bool Invert(const Matrix m, Matrix out) {
    float a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0f : 0.0f);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-12f) return false;
        if (p != c)
            for (int k = 0; k < 8; k++) std::swap(a[p][k], a[c][k]);
        const float inv = 1.0f / a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] *= inv;
        for (int r = 0; r < 4; r++)
            if (r != c) {
                const float f = a[r][c];
                if (f != 0)
                    for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r * 4 + c] = a[r][c + 4];
    return true;
}

void ReadMatrix(uint32_t va, Matrix m) { std::memcpy(m, &kernel::G<uint8_t>(va), sizeof(Matrix)); }

// the car's world matrix (row vectors: a point of the car x it is the world's), its last column made 0 0 0 1
void CarWorld(X86Context& ctx, uint32_t car, Matrix m) {
    ctx.ecx = car;
    ReadMatrix(kernel::CallGuest(ctx, R32(R32(car) + kCarWorld), {}), m);
    m[3] = m[7] = m[11] = 0, m[15] = 1;
}

bool EnsureTiles(X86Context& ctx) {
    if (!g_tiles.empty()) return true;
    if (g_tiles_failed) return false;
    g_tiles_failed = true;
    static const uint32_t want = std::getenv("PGR2_TILE") ? uint32_t(std::atoi(std::getenv("PGR2_TILE"))) : 256;
    const uint32_t size = R32(kMapSize);
    if (!size) return false;
    g_tile_size = std::clamp<uint32_t>(want, (size + 3) / 4, size);  // 0x1892E0 keeps its scale at least 0.25
    // the renderer takes any depth target as 4 bytes a pixel (metal.mm EnsureTarget) and reads that much of it: room for it
    g_tile_bytes = (0x1000 + g_tile_size * g_tile_size * 4 + 0xFFF) & ~0xFFFu;
    const uint32_t slots = std::min<uint32_t>(kTileArenaBytes / g_tile_bytes, 8 * kViews);
    g_tiles.resize(slots);
    g_upload = mem::AllocSystem(sizeof(Matrix), 16);
    g_tiles_failed = false;
    LOGR("street light shadows: %u cached map tiles of %ux%u (the map is %u)", slots, g_tile_size, g_tile_size, size);
    return true;
}

// a slot's headers and pixels, made the first time it is used
// colour_on_data: the colour surface on the depth's memory (the map writes no colour, so the renderer draws depth alone), for slots
// the renderer draws at a fixed scale: a colour surface with no memory would be made at the window's, and the renderer leaves out a
// depth target of another size than the colour's
bool PrepareSlotAt(X86Context& ctx, Tile& t, uint32_t at, uint32_t bytes, uint32_t s, bool colour_on_data = false) {
    if (t.surface) return true;
    if (!mem::Commit(at, bytes)) return false;
    t.surface = at, t.colour = at + 0x40, t.texture = at + 0x80, t.data = at + 0x1000;
    std::memset(&kernel::G<uint8_t>(t.data), 0xFF, s * s * 2);  // the far plane: a lost target casts nothing
    kernel::CallGuest(ctx, kSetSurfaceHeader, {s, s, kFormatDepth, t.surface, t.data, s * 2});
    kernel::CallGuest(ctx, kSetSurfaceHeader, {s, s, kFormatColour, t.colour, colour_on_data ? t.data : 0u, 0});
    kernel::CallGuest(ctx, kSetTextureHeader, {s, s, 1, 0, kFormatDepth, 0, t.texture, t.data, s * 2});
    return true;
}
bool PrepareSlot(X86Context& ctx, Tile& t, size_t index) {
    return PrepareSlotAt(ctx, t, kTileArena + uint32_t(index) * g_tile_bytes, g_tile_bytes, g_tile_size);
}

// the car's depth map with the light travelling along dir (world), drawn by 0x1892E0 into a tile's surfaces of size x size (the
// car filling it): true when its texture matrix (view^-1 x light view x projection x bias) was caught, in g_captured_m
bool DrawMap(X86Context& ctx, const Tile& t, uint32_t size, uint32_t car, const float dir[3]) {
    uint8_t sun[12];
    std::memcpy(sun, &kernel::G<uint8_t>(kSunDirection), 12);
    std::memcpy(&kernel::G<uint8_t>(kSunDirection), dir, 12);
    const uint32_t surface = R32(kMapSurface), colour = R32(kMapColour), map_size = R32(kMapSize);
    kernel::W32(kMapSurface, t.surface);
    kernel::W32(kMapColour, t.colour);
    // 0x1892E0 draws a viewport of the map's size x the scale, but widens its view by 1 / scale too (0x1898EC), so a smaller
    // scale leaves the car in the middle of the viewport: the map's size is made the tile's for the draw, at scale 1, and the car
    // fills the tile (4x the texels of a 512 tile at scale 0.5)
    static const bool fill = !std::getenv("PGR2_TILE_SCALED");  // testing: the old way
    const float scale = fill ? 1.0f : float(size) / float(map_size);
    if (fill) kernel::W32(kMapSize, size);
    uint32_t scale_bits;
    std::memcpy(&scale_bits, &scale, 4);
    g_capture = true, g_captured = false;
    g_in_map = true;
    ctx.ecx = kShadowObject;
    kernel::CallGuest(ctx, kDrawMap, {car, scale_bits});
    kernel::W32(kMapSize, map_size);
    g_in_map = false;
    g_capture = false;
    kernel::W32(kMapSurface, surface);
    kernel::W32(kMapColour, colour);
    std::memcpy(&kernel::G<uint8_t>(kSunDirection), sun, 12);
    return g_captured;
}

// the car's map along a view, drawn into a tile: true when the matrix was caught
bool MakeTile(X86Context& ctx, Tile& t, uint32_t car, int view) {
    Matrix world;
    CarWorld(ctx, car, world);
    float local[3], dir[3];
    ViewDirection(view, local);
    for (int j = 0; j < 3; j++) dir[j] = local[0] * world[j] + local[1] * world[4 + j] + local[2] * world[8 + j];
    const float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    for (float& c : dir) c = dl > 0 ? c / dl : 0;
    if (!DrawMap(ctx, t, g_tile_size, car, dir)) return false;
    Matrix view_m, a;
    ReadMatrix(R32(kCamera) + kCameraView, view_m);
    Multiply(world, view_m, a);
    Multiply(a, g_captured_m, t.k);
    t.car = car, t.view = view, t.made = g_frame, t.ready = true;
    return true;
}

// the car's tile for a view: made now if the frame's budget allows, else the car's nearest to the light's way (local, in the car's
// space) within about two views
Tile* FindTile(X86Context& ctx, uint32_t car, const float local[3], int view) {
    static const uint32_t budget = std::getenv("PGR2_TILE_BUDGET") ? uint32_t(std::atoi(std::getenv("PGR2_TILE_BUDGET"))) : 4;
    // a tile is drawn again after this many frames (the car's damage; a target lost); testing: PGR2_TILE_REFRESH
    static const uint32_t refresh = std::getenv("PGR2_TILE_REFRESH") ? uint32_t(std::atoi(std::getenv("PGR2_TILE_REFRESH"))) : 60 * 20;
    Tile* have = nullptr;
    for (Tile& t : g_tiles)
        if (t.ready && t.car == car && t.view == view) have = &t;
    if (have && g_frame - have->made < refresh) return have;
    if (g_made_this_frame < budget) {
        Tile* slot = have;
        if (!slot) {  // a free slot, else the least recently used
            size_t best = 0;
            for (size_t i = 0; i < g_tiles.size(); i++) {
                if (!g_tiles[i].ready) {
                    best = i;
                    break;
                }
                if (g_tiles[i].used < g_tiles[best].used) best = i;
            }
            slot = &g_tiles[best];
            if (!PrepareSlot(ctx, *slot, best)) return nullptr;
        }
        g_made_this_frame++;
        slot->ready = false;
        if (MakeTile(ctx, *slot, car, view)) return slot;
        return nullptr;
    }
    if (have) return have;
    // the nearest of the car's tiles, if near enough (about two views off)
    Tile* near = nullptr;
    float best = 0.9f;
    for (Tile& t : g_tiles) {
        if (!t.ready || t.car != car) continue;
        float d[3];
        ViewDirection(t.view, d);
        const float dot = d[0] * local[0] + d[1] * local[1] + d[2] * local[2];
        if (dot > best) best = dot, near = &t;
    }
    return near;
}

// the car's ground mesh (0x21920) reading a map of ours: its texture matrix (not transposed) at c[0x8F], its texture in the map's
// place; pcf filters stage 0 for 2x2 comparisons
void DrawMesh(X86Context& ctx, uint32_t car, uint32_t texture_header, const Matrix matrix, bool pcf) {
    // the camera's own transforms again, as 0x1892E0 ends with (0x189ADC): the night pass's flattened shadows load their world
    // the states 0x1892E0 leaves, which the ground mesh draws with (the night pass's own differ): culling 0x900 (set for the car's
    // depth at 0x189A3D, never put back) and Z as W (0x189ABE); each through the game's copy of the state, as it sets them
    if (kernel::R16(kCullModeCopy) != kCullMap) {
        kernel::W16(kCullModeCopy, kCullMap);
        kernel::CallGuest(ctx, kSetCullMode, {kCullMap});
    }
    if (kernel::G<uint8_t>(kZEnableCopy) != kZEnableW) {
        kernel::G<uint8_t>(kZEnableCopy) = kZEnableW;
        kernel::CallGuest(ctx, kSetZEnable, {kZEnableW});
    }
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
    // stage 0 filtered (the renderer then compares 2x2 texels and blends them: a softer edge than the tile's texels); the stage's
    // filters as they were afterwards, so the game's own copy of them stays true. 0x21920 sets only its addressing (0x20580)
    const uint32_t mag = R32(kStage0 + kTssMag * 4), min = R32(kStage0 + kTssMin * 4);
    if (pcf) {
        kernel::W32(kStage0 + kTssMag * 4, kTexfLinear);
        kernel::W32(kStage0 + kTssMin * 4, kTexfLinear);
        kernel::W32(kDirtyFlags, R32(kDirtyFlags) | kDirtyStage0);
    }
    ctx.ecx = car;
    kernel::CallGuest(ctx, kGroundMesh, {});
    if (pcf) {
        kernel::W32(kStage0 + kTssMag * 4, mag);
        kernel::W32(kStage0 + kTssMin * 4, min);
        kernel::W32(kDirtyFlags, R32(kDirtyFlags) | kDirtyStage0);
    }
    kernel::W32(kMapTexture, texture);
}

// the ground mesh with a tile: its matrix for this frame at c[0x8F], its texture in the map's place
// The tile's silhouette is the car seen along its view; the light's own way differs (up to a band in elevation, half an azimuth's
// spacing). Each ground point p (in the car's space) is moved to where the view's ray through a slice of the car at height h meets
// p's level, the slice point being where the light's ray from p crosses it: p' = p + (h - p.y) (D / D.y - V / V.y). Exact for
// that slice, h half the car's height over the road. (The roof, which fits a box-shaped car's shadow length exactly with a view
// below the light, matched mode 3 worse on a recorded race: 1.4 against 0.9 mean error; the cross-fade does more for the jumps.)
void ShearToward(const float light[3], int view, float h, Matrix out) {
    float v[3];
    ViewDirection(view, v);
    const float ly = std::min(light[1], -0.1f), vy = std::min(v[1], -0.1f);
    const float sx = light[0] / ly - v[0] / vy, sz = light[2] / ly - v[2] / vy;
    for (int i = 0; i < 16; i++) out[i] = i % 5 == 0 ? 1.0f : 0.0f;
    out[4] = -sx, out[6] = -sz;     // row y: x' -= y sx, z' -= y sz
    out[12] = h * sx, out[14] = h * sz;
}

void DrawWithTile(X86Context& ctx, uint32_t car, Tile& t, const float light[3]) {
    Matrix world, world_inv, view_m, view_inv, a, b, shear, sheared;
    CarWorld(ctx, car, world);
    ReadMatrix(R32(kCamera) + kCameraView, view_m);
    if (!Invert(world, world_inv) || !Invert(view_m, view_inv)) return;
    Multiply(view_inv, world_inv, a);
    static const bool no_shear = std::getenv("PGR2_TILE_NO_SHEAR") != nullptr;  // testing
    static const float slice = std::getenv("PGR2_TILE_SLICE") ? float(std::atof(std::getenv("PGR2_TILE_SLICE"))) : 0.5f;
    if (!no_shear) {
        // the road's height in the car's space: the night pass's plane under the car (normalised, facing up) at the car's place
        float n[4];
        const float nl = std::sqrt(g_plane[0] * g_plane[0] + g_plane[1] * g_plane[1] + g_plane[2] * g_plane[2]);
        for (int i = 0; i < 4; i++) n[i] = nl > 0 ? g_plane[i] / nl * (g_plane[1] < 0 ? -1.0f : 1.0f) : 0;
        const float above = n[0] * world[12] + n[1] * world[13] + n[2] * world[14] + n[3];
        const float ground = nl > 0 && std::fabs(above) < 3.0f ? -above : 0.0f;
        const float roof = F(car + kCarRoof);
        ShearToward(light, t.view, ground + slice * (roof > 0.3f && roof < 4.0f ? roof : 1.3f), shear);
        Multiply(shear, t.k, sheared);
        Multiply(a, sheared, b);
    } else {
        Multiply(a, t.k, b);
    }
    // testing: PGR2_TILE_LOG, the matrix against the one the game made with the tile, when made this frame
    static const bool log = std::getenv("PGR2_TILE_LOG") != nullptr;
    static int said = 0;
    if (log && t.made == g_frame && said < 12) {
        said++;
        float diff = 0;
        for (int i = 0; i < 16; i++) diff = std::max(diff, std::fabs(b[i] - g_captured_m[i]));
        LOGR("street light shadows: tile %d of car %08X, matrix off by %.5f; game's row 3 %.3f %.3f %.3f %.3f, ours %.3f %.3f %.3f %.3f", t.view, car,
             diff, g_captured_m[12], g_captured_m[13], g_captured_m[14], g_captured_m[15], b[12], b[13], b[14], b[15]);
    }
    static const bool pcf = !std::getenv("PGR2_TILE_POINT");  // testing
    DrawMesh(ctx, car, t.texture, b, pcf);
    t.used = g_frame;
}

// ---- mode 3, batched: the night pass only notes each light's shadow (car, the light's way, its colour, its mesh radius); after
// the pass every map is drawn, one after another, into a slot of its own, and then every ground mesh, each with its map. Each map
// drawn in the pass's middle sent the renderer from the frame's target to the map and back, a new command buffer each way (about
// 44 a frame against the console's 13 in a recorded race); now once a batch. The maps and meshes are the ones mode 3 drew: the
// same look. Slots of the map's size (1024) in kBatchArena: 23; a frame with more lights runs more batches. Drawn at a fixed scale
// (gpu::g_fixed_scale, EnsureBatch), else each is a target of the window's scale, 3413 across at 3.33. Measured, a recorded race:
// command buffers 44 -> 13 a frame (75 -> 15 with 6+ cars), work p95 12.6 -> 11.9 ms on an idle machine, about half the work
// when the machine was busy with another game running.
struct Pending {
    uint32_t car, colour;
    float dir[3], radius;
};
std::vector<Pending> g_pending;
std::vector<Tile> g_batch;
uint32_t g_batch_size = 0, g_batch_bytes = 0;
bool g_batch_failed = false;
Matrix g_batch_matrix[64];

bool EnsureBatch(X86Context& ctx) {
    if (!g_batch.empty()) return true;
    if (g_batch_failed) return false;
    g_batch_failed = true;
    static const uint32_t want = std::getenv("PGR2_BATCH_TILE") ? uint32_t(std::atoi(std::getenv("PGR2_BATCH_TILE"))) : 0;
    const uint32_t size = R32(kMapSize);
    if (!size) return false;
    g_batch_size = want ? std::clamp<uint32_t>(want, 128, 2048) : size;
    g_batch_bytes = (0x1000 + g_batch_size * g_batch_size * 4 + 0xFFF) & ~0xFFFu;
    const uint32_t slots = std::min<uint32_t>(kBatchArenaBytes / g_batch_bytes, 64);
    if (!slots) return false;
    g_batch.resize(slots);
    if (!g_upload) g_upload = mem::AllocSystem(sizeof(Matrix), 16);
    g_batch_failed = false;
    // drawn at a fixed scale, not the window's (3.33 at full screen made each 1024 map 3413 pixels across, and too big for the
    // renderer to keep its clear in the batch's command buffer): PGR2_BATCH_SCALE pixels a guest pixel, 1.25 by default (up to 1400 across keeps the clear in the batch)
    static const float fixed = std::getenv("PGR2_BATCH_SCALE") ? float(std::atof(std::getenv("PGR2_BATCH_SCALE"))) : 1.25f;
    if (fixed > 0) {
        gpu::g_fixed_scale = fixed;
        gpu::g_fixed_scale_lo = kBatchArena;
        gpu::g_fixed_scale_hi = kBatchArena + kBatchArenaBytes;
    }
    LOGR("street light shadows: mode 3 batches of %u maps of %ux%u, drawn at %.2f", slots, g_batch_size, g_batch_size, fixed);
    return true;
}

// the noted shadows drawn: maps, then meshes, a batch at a time
void FlushBatch(X86Context& ctx) {
    if (g_pending.empty()) return;
    if (!EnsureBatch(ctx)) {
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
            Tile& t = g_batch[i - start];
            const Pending& p = g_pending[i];
            ok[i - start] = PrepareSlotAt(ctx, t, kBatchArena + uint32_t(i - start) * g_batch_bytes, g_batch_bytes, g_batch_size, true) &&
                            DrawMap(ctx, t, g_batch_size, p.car, p.dir);
            if (ok[i - start]) std::memcpy(g_batch_matrix[i - start], g_captured_m, sizeof(Matrix));
        }
        for (size_t i = start; i < end; i++) {
            if (!ok[i - start]) continue;
            const Pending& p = g_pending[i];
            std::memcpy(&kernel::G<uint8_t>(kSunDirection), p.dir, 12);  // the mesh's shade: base / (base + colour x -dir.y)
            kernel::W32(kSunColour, p.colour);
            pgr2::g_shadow_mark = pgr2::g_shadow_mark % 255 + 1;
            g_fit_radius = p.radius;
            DrawMesh(ctx, p.car, g_batch[i - start].texture, g_batch_matrix[i - start], false);
        }
    }
    g_fit_radius = 0;
    g_inner = false;
    pgr2::cpu_car_shadows = cpu;
    std::memcpy(&kernel::G<uint8_t>(kSunDirection), sun, 12);
    kernel::W32(kSunColour, sun_colour);
    g_pending.clear();
}

}  // namespace

// the night car shadows (thiscall on the car list)
X86_FUNC(sub_00032D10) {
    static const bool none = std::getenv("PGR2_NO_NIGHT_CARS") != nullptr;  // testing: the pass left out
    if (none) {
        ctx.esp += 4;
        return;
    }
    g_pass = true;
    g_have = false;
    last_car = 0;
    g_frame++;
    g_made_this_frame = 0;
    // reach: each light's radius (and the inner one where it is at full strength) widened for this pass only, so lights a little
    // farther off still cast, and stronger: 0x32D10 keeps a light within its radius (+0x28) of the car, at a strength (0x409E0)
    // falling from the inner radius (+0x2C) to the outer
    const uint32_t reach = pgr2::street_light_shadows ? std::clamp<uint32_t>(pgr2::street_light_reach, 50, 400) : 100;
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

// D3DXMatrixShadow (out, light, plane; stdcall): in the night pass, its plane and light are kept, and the light's place taken
// from the record being looked at (esi, its offset in the array)
X86_FUNC(sub_00194C8C) {
    if (g_pass && R32(ctx.esp) == kFromShadowMatrix) {
        const uint32_t light = R32(ctx.esp + 8), plane = R32(ctx.esp + 12), record = R32(kLights) + ctx.esi;
        for (int i = 0; i < 4; i++) g_plane[i] = F(plane + i * 4), g_dir[i] = F(light + i * 4);
        for (int i = 0; i < 3; i++) g_lamp[i] = F(record + kLightPlace + i * 4);
    }
    __imp__sub_00194C8C(ctx, base);
}

// 0x30AE0 (thiscall on a matrix: k; ret 4) divides it by k: the game's shadow matrix, normalised before it is used
X86_FUNC(sub_00030AE0) {
    if (g_pass && R32(ctx.esp) == kFromScale) g_divide = F(ctx.esp + 4);
    __imp__sub_00030AE0(ctx, base);
}

// D3DXMatrixMultiply (out, a, b; stdcall): the car's world matrix is a
X86_FUNC(sub_0019430F) {
    if (g_pass && R32(ctx.esp) == kFromMultiply) {
        std::memcpy(g_world, &kernel::G<uint8_t>(R32(ctx.esp + 8)), sizeof(Matrix));
        g_have = true;
    }
    __imp__sub_0019430F(ctx, base);
}

// the car's shadow mesh drawn (thiscall on the mesh: shade; ret 4)
X86_FUNC(sub_000C7C00) {
    // testing: PGR2_TILE_ALTERNATE=1, mode 3 and 4 on alternate frames (one run, the same moments; shots at consecutive reads)
    static const bool alternate = std::getenv("PGR2_TILE_ALTERNATE") != nullptr;
    static uint32_t frames_seen = 0, frame_was = 0;
    if (alternate && g_frame != frame_was && g_pass) frame_was = g_frame, frames_seen++;
    const uint32_t mode = alternate && pgr2::street_light_shadows >= 3 ? (g_frame / 2 % 2 ? 3u : 4u) : pgr2::street_light_shadows;
    if (!g_inner && g_pass && R32(ctx.esp) == kFromDraw) {
        // which of its car's lights this is (testing: PGR2_ONLY_LIGHT), and the counts for the perf recorder
        const uint32_t this_car = ctx.ebx - kCarFromPointer;
        light_index = this_car == last_car ? light_index + 1 : 0;
        last_car = this_car;
        if (perf::On()) {
            pgr2::g_night_shadows.fetch_add(1, std::memory_order_relaxed);
            pgr2::g_night_lights.fetch_add(1, std::memory_order_relaxed);
            if (light_index == 0) pgr2::g_night_cars.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (g_inner || !g_pass || !g_have || mode == 0 || R32(ctx.esp) != kFromDraw) {
        __imp__sub_000C7C00(ctx, base);
        return;
    }
    g_have = false;
    perf::Zone light_zone(pgr2::kZStreetLight);
    static const int only = std::getenv("PGR2_ONLY_LIGHT") ? std::atoi(std::getenv("PGR2_ONLY_LIGHT")) : -1;
    if (only >= 0 && int(light_index) != only) {
        ctx.esp += 8;
        return;
    }
    if (mode >= 3) {
        // the day's machinery, pointed at the lamp: the car's depth map drawn from it, then its ground mesh with that map, shaded
        // as this light's shadow. The ground mesh's shade is base / (base + colour x -dir.y) per channel; the colour is chosen so
        // that it equals the night pass's shade for this light
        const uint32_t car = ctx.ebx - kCarFromPointer, shade = R32(ctx.esp + 4);
        float dir[3] = {F(car + 0x44) - g_lamp[0], F(car + 0x48) - g_lamp[1], F(car + 0x4C) - g_lamp[2]};
        const float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        for (float& c : dir) c = dl > 0 ? c / dl : 0;
        const float lift = std::max(-dir[1], 0.05f);  // the sun's colour is scaled by this
        // the colour for a pass carrying this share of the shadow (shares multiply: two passes of 1/2 each are one whole)
        auto colour_for = [&](float share) {
            uint32_t colour = 0;
            for (int c = 0; c < 4; c++) {  // channels as 0x21920 pairs them: ambient +0, +4, +8, +C with colour bytes 16, 8, 0, 24
                const int shift = c == 0 ? 16 : c == 1 ? 8 : c == 2 ? 0 : 24;
                // the flattened mesh is a closed hull whose fans overlap a few layers deep, each multiplying: the console's shadow is
                // about the shade to the power kDarken where it is darkest; the map's mesh multiplies once, so it is given that
                static const float darken = std::getenv("PGR2_MAP_DARKEN") ? float(std::atof(std::getenv("PGR2_MAP_DARKEN"))) : 4.0f;
                const float want = std::clamp(std::pow(float(shade >> shift & 0xFF) / 255.0f, darken * share), 0.02f, 1.0f);
                const float ambient = F(car + kCarAmbient + c * 4);
                const float light = ambient * (1.0f / want - 1.0f) / lift;  // colour x lift = ambient (1 / shade - 1)
                colour |= uint32_t(std::clamp(std::lround(light * 255.0f), 0L, 255L)) << shift;
            }
            return colour;
        };
        const uint32_t colour = colour_for(1.0f);
        uint8_t sun[12];
        std::memcpy(sun, &kernel::G<uint8_t>(kSunDirection), 12);
        const uint32_t sun_colour = R32(kSunColour);
        std::memcpy(&kernel::G<uint8_t>(kSunDirection), dir, 12);
        kernel::W32(kSunColour, colour);
        pgr2::g_shadow_mark = pgr2::g_shadow_mark % 255 + 1;  // each light's shadow its own (draw_filter: one shadow a mark)
        const bool cpu = pgr2::cpu_car_shadows;
        pgr2::cpu_car_shadows = false;  // the mesh of this car alone (car_shadows.cpp would add the others')
        g_inner = true;
        // PGR2_MESH_FIT=1: the ground mesh only as far as this shadow reaches: from the car's middle, half its length and the car's
        // height x the lamp's distance across the road / (the lamp's height over the car's roof)
        static const bool fit = std::getenv("PGR2_MESH_FIT") != nullptr;
        g_fit_radius = 0;
        if (fit) {
            const float across = std::sqrt((g_lamp[0] - F(car + 0x44)) * (g_lamp[0] - F(car + 0x44)) + (g_lamp[2] - F(car + 0x4C)) * (g_lamp[2] - F(car + 0x4C)));
            const float roof = F(car + 0xB0), over = std::max(g_lamp[1] - F(car + 0x48) - roof, 0.5f);
            g_fit_radius = std::clamp(F(car + 0xAC) * 0.5f + roof * across / over + 0.5f, 2.5f, 12.0f);
        }
        const uint64_t t0 = perf::Ticks();
        if (mode >= 4 && EnsureTiles(ctx)) {
            // the cached map nearest the light's way in the car's space (its world's rows are the car's axes)
            Matrix world;
            CarWorld(ctx, car, world);
            float local[3];
            for (int i = 0; i < 3; i++) {
                const float* row = world + i * 4;
                const float n2 = row[0] * row[0] + row[1] * row[1] + row[2] * row[2];
                local[i] = n2 > 0 ? (row[0] * dir[0] + row[1] * dir[1] + row[2] * dir[2]) / n2 : 0;
            }
            // the views either side of the light's way, each with its share of the shadow (ChooseViews). Each pass has a stencil
            // mark of its own, so the passes compound where they overlap and their shares multiply to the whole shadow
            Pick picks[4];
            int count = ChooseViews(local, picks);
            Tile* tiles[4] = {};
            const uint32_t made_before = g_made_this_frame;
            float total = 0;
            int kept = 0;
            for (int k = 0; k < count; k++)
                if (Tile* t = FindTile(ctx, car, local, picks[k].view)) tiles[kept] = t, picks[kept] = picks[k], total += picks[k].share, kept++;
            count = kept;
            (count ? (g_made_this_frame != made_before ? g_tiles_made : g_tiles_reused) : g_tiles_missing)++;
            if (count > 1) g_tiles_faded++;
            static const bool trace_log = std::getenv("PGR2_TILE_TRACE_LOG") != nullptr;  // testing: the followed car's lights in the log
            if (trace_log && car == kFirstCar)
                LOGR("tile trace: frame %u lamp %.1f %.1f %.1f shade %02X views %d %.2f, %d %.2f, %d %.2f, %d %.2f, made %u", g_frame, g_lamp[0],
                     g_lamp[1], g_lamp[2], shade & 0xFF, count > 0 ? tiles[0]->view : -1, count > 0 ? picks[0].share : 0, count > 1 ? tiles[1]->view : -1,
                     count > 1 ? picks[1].share : 0, count > 2 ? tiles[2]->view : -1, count > 2 ? picks[2].share : 0, count > 3 ? tiles[3]->view : -1,
                     count > 3 ? picks[3].share : 0, g_made_this_frame - made_before);
            const uint64_t t1 = perf::Ticks();
            for (int k = 0; k < count; k++) {
                if (k > 0) pgr2::g_shadow_mark = pgr2::g_shadow_mark % 255 + 1;
                kernel::W32(kSunColour, count > 1 ? colour_for(picks[k].share / total) : colour);
                DrawWithTile(ctx, car, *tiles[k], local);
            }
            g_map_ticks += t1 - t0, g_mesh_ticks += perf::Ticks() - t1;
        } else if (static const bool batch = !std::getenv("PGR2_NO_BATCH"), alt = std::getenv("PGR2_BATCH_ALTERNATE") != nullptr;
                   batch && !(alt && g_frame / 2 % 2)) {  // testing: PGR2_BATCH_ALTERNATE, batched on alternate frames
            // noted, drawn after the pass with the others (FlushBatch)
            Pending p;
            p.car = car, p.colour = colour, p.radius = g_fit_radius;
            std::memcpy(p.dir, dir, sizeof(p.dir));
            g_pending.push_back(p);
        } else {
            ctx.ecx = kShadowObject;
            g_in_map = true;
            kernel::CallGuest(ctx, kDrawMap, {car, 0x3F800000u});
            g_in_map = false;
            const uint64_t t1 = perf::Ticks();
            ctx.ecx = car;
            kernel::CallGuest(ctx, kGroundMesh, {});
            g_map_ticks += t1 - t0, g_mesh_ticks += perf::Ticks() - t1;
        }
        g_inner = false;
        pgr2::cpu_car_shadows = cpu;
        std::memcpy(&kernel::G<uint8_t>(kSunDirection), sun, 12);
        kernel::W32(kSunColour, sun_colour);
        // testing: PGR2_HL_LOG=1 logs every map-and-mesh pair for a while, then a count every 2 s
        static const bool log = std::getenv("PGR2_HL_LOG") != nullptr;
        static int said = 0;
        static uint32_t pairs = 0;
        static auto since = std::chrono::steady_clock::now();
        pairs++;
        if (log && said < 40) {
            said++;
            LOGR("street light shadows: map from the lamp, car %08X at %.1f %.1f %.1f, lamp %.1f %.1f %.1f, direction %.2f %.2f %.2f, shade %08X as "
                 "colour %08X, ambient %.3f %.3f %.3f", car, F(car + 0x44), F(car + 0x48), F(car + 0x4C), g_lamp[0], g_lamp[1], g_lamp[2], dir[0],
                 dir[1], dir[2], shade, colour, F(car + kCarAmbient), F(car + kCarAmbient + 4), F(car + kCarAmbient + 8));
        }
        if (log && std::chrono::steady_clock::now() - since > std::chrono::seconds(2)) {
            const double tick_ms = 1000.0 / 24e6;  // the ARM counter (perf::Ticks)
            LOGR("street light shadows: %u maps and meshes drawn in 2 s; per shadow, map %.3f ms, mesh %.3f ms (game thread)", pairs,
                 pairs ? g_map_ticks * tick_ms / pairs : 0.0, pairs ? g_mesh_ticks * tick_ms / pairs : 0.0);
            if (mode >= 4) LOGR("street light shadows: tiles made %u, reused %u, missing %u; cross-faded %u", g_tiles_made, g_tiles_reused, g_tiles_missing, g_tiles_faded);
            g_tiles_made = g_tiles_reused = g_tiles_missing = g_tiles_faded = 0;
            pairs = 0;
            g_map_ticks = g_mesh_ticks = 0;
            since = std::chrono::steady_clock::now();
        }
        ctx.esp += 8;  // ret 4
        return;
    }
    // testing: PGR2_SHADOW_SAMPLES, PGR2_LAMP_RADIUS
    static const int kSamples = std::getenv("PGR2_SHADOW_SAMPLES") ? std::max(1, std::atoi(std::getenv("PGR2_SHADOW_SAMPLES"))) : kSamplesDefault;
    static const float kLampRadius = std::getenv("PGR2_LAMP_RADIUS") ? float(std::atof(std::getenv("PGR2_LAMP_RADIUS"))) : kLampRadiusDefault;
    if (!g_scratch) g_scratch = mem::AllocSystem(sizeof(Matrix), 16);
    const uint32_t mesh = ctx.ecx, shade = R32(ctx.esp + 4);

    // the plane normalised, the lamp's height over it kept at least kMinHeight
    float plane[4];
    const float n = std::sqrt(g_plane[0] * g_plane[0] + g_plane[1] * g_plane[1] + g_plane[2] * g_plane[2]);
    for (int i = 0; i < 4; i++) plane[i] = n > 0 ? g_plane[i] / n : g_plane[i];
    float lamp[3] = {g_lamp[0], g_lamp[1], g_lamp[2]};
    const float height = plane[0] * lamp[0] + plane[1] * lamp[1] + plane[2] * lamp[2] + plane[3];
    if (height < kMinHeight)
        for (int i = 0; i < 3; i++) lamp[i] += plane[i] * (kMinHeight - height) * (height >= 0 ? 1.0f : -1.0f);
    // two directions across the plane for the lamp's disc
    float u[3] = {plane[1], -plane[0], 0};  // horizontal when the plane is level
    if (std::fabs(u[0]) + std::fabs(u[1]) < 1e-3f) u[0] = 1, u[1] = 0, u[2] = 0;
    const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (float& c : u) c /= ul;
    const float v[3] = {plane[1] * u[2] - plane[2] * u[1], plane[2] * u[0] - plane[0] * u[2], plane[0] * u[1] - plane[1] * u[0]};

    static int said = 0;
    if (said++ < 3)
        LOGR("street light shadows: mode %u, lamp %.1f %.1f %.1f, %.1f m over the road, the game's direction %.2f %.2f %.2f, divided by %.3f, shade %08X",
             mode, g_lamp[0], g_lamp[1], g_lamp[2], height, g_dir[0], g_dir[1], g_dir[2], g_divide, shade);

    // each pass darkens by its vertices' colours to the power 1 / kSamples (the draw mixes the shade with a base colour by height,
    // so the colours are scaled as they are set): where every pass's shadow falls the road is as dark as the game's one shadow
    g_inner = true;
    pgr2::g_shade_power = 1.0f / kSamples;
    for (int s = 0; s < kSamples; s++) {
        const float angle = kSamples > 1 ? float(s - 1) * 6.2831853f / float(kSamples - 1) : 0.0f;
        const float r = s == 0 ? 0.0f : kLampRadius;
        float at[3];
        for (int i = 0; i < 3; i++) at[i] = lamp[i] + r * (std::cos(angle) * u[i] + std::sin(angle) * v[i]);
        float light[4];
        if (mode >= 2) {
            light[0] = at[0], light[1] = at[1], light[2] = at[2], light[3] = 1;  // a point
        } else {
            // a direction, as the game's: from the car (its world matrix's place) to this point of the lamp
            float dir[3] = {at[0] - g_world[12], at[1] - g_world[13], at[2] - g_world[14]};
            const float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            for (float& c : dir) c = dl > 0 ? c / dl : 0;
            light[0] = dir[0], light[1] = dir[1], light[2] = dir[2], light[3] = 0;
        }
        Matrix shadow, m;
        ShadowMatrix(plane, light, shadow);
        // the draw is W-buffered: its depth is the w it ends with, and a point on the road keeps the shadow matrix's d as its w.
        // The game divides its matrix by g_divide (0x30AE0) before use; each of ours is scaled to end as the game's would (the same
        // point on the screen, the same depth): times d_game / d, then divided as the game's
        const float game_d = plane[0] * g_dir[0] + plane[1] * g_dir[1] + plane[2] * g_dir[2] + plane[3] * g_dir[3];
        const float d = plane[0] * light[0] + plane[1] * light[1] + plane[2] * light[2] + plane[3] * light[3];
        const float scale = (std::fabs(d) > 1e-6f ? game_d / d : 1.0f) / (std::fabs(g_divide) > 1e-6f ? g_divide : 1.0f);
        for (float& e : shadow) e *= scale;
        Multiply(g_world, shadow, m);
        std::memcpy(&kernel::G<uint8_t>(g_scratch), m, sizeof(Matrix));
        kernel::CallGuest(ctx, kLoadWorld, {g_scratch});
        ctx.ecx = mesh;
        kernel::CallGuest(ctx, kDraw, {shade});
    }
    pgr2::g_shade_power = 0;
    g_inner = false;
    ctx.esp += 8;  // ret 4
}


// the ground mesh's road triangles (thiscall on 0x381910: 0, car, radius, 4; ret 0x10): for a street light's shadow the radius
// grows with the reach, as shadows from farther lamps are longer than the day's 4 m
X86_FUNC(sub_000BF3F0) {
    if (g_inner) {
        const float radius = g_fit_radius > 0 ? g_fit_radius
                                              : std::min(F(ctx.esp + 12) * float(std::clamp<uint32_t>(pgr2::street_light_reach, 100, 300)) / 100.0f, 12.0f);
        WF(ctx.esp + 12, radius);
    }
    __imp__sub_000BF3F0(ctx, base);
}

// the car's model drawn (thiscall: level of detail, pass, ?; the depth pass is (0, 2, 2)): testing, PGR2_MAP_LOD=n draws a street
// light's map at level n (the same draws, fewer triangles: detail.cpp)
X86_FUNC(sub_00022470) {
    static const int lod = std::getenv("PGR2_MAP_LOD") ? std::atoi(std::getenv("PGR2_MAP_LOD")) : -1;
    if (g_in_map && lod >= 0) kernel::W32(ctx.esp + 4, uint32_t(lod));
    __imp__sub_00022470(ctx, base);
}

// D3DXMatrixTranspose (out, in; stdcall): in a cached map's draw (mode 4) the texture matrix 0x1892E0 is about to upload is kept
X86_FUNC(sub_0019440D) {
    if (g_capture && R32(ctx.esp) == kFromTranspose) {
        ReadMatrix(R32(ctx.esp + 8), g_captured_m);
        g_captured = true;
    }
    __imp__sub_0019440D(ctx, base);
}
