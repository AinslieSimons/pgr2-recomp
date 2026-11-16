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
// Cost: one depth map and mesh per car per light, 300 to 800 a second in a race: a frame rate dip.
// Each matrix is built as the XDK's D3DXMatrixShadow builds it (its sign is the opposite of the D3DX documentation's) and scaled to
// end with the w the game's own ends with (0x30AE0 divides the game's by d): the draw is W-buffered, so w is its depth, and a
// different w put the shadow behind the road or behind the camera. With one sample, no radius and mode 1 the matrix equals the
// game's to 4 decimals (checked). For testing: PGR2_STREET_SHADOWS, PGR2_STREET_REACH, PGR2_SHADOW_SAMPLES, PGR2_LAMP_RADIUS,
// PGR2_MAP_DARKEN, PGR2_ONLY_LIGHT (one light's shadow per car), PGR2_NO_NIGHT_CARS, PGR2_HL_LOG.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "app/perf.h"
#include "core/log.h"
#include "game/perf_zones.h"
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
constexpr float kMinHeight = 3.0f;               // a lamp nearer the road plane than this is treated as this high (no endless shadows)

using Matrix = float[16];  // D3DX layout: row vectors, rows of 4

bool g_pass = false;     // inside 0x32D10
bool g_inner = false;    // our own calls of the draw
uint32_t last_car = 0, light_index = 0;  // testing: which of a car's lights this is, in the pass
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
    const uint32_t mode = pgr2::street_light_shadows;
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
        uint32_t colour = 0;
        for (int c = 0; c < 4; c++) {  // channels as 0x21920 pairs them: ambient +0, +4, +8, +C with colour bytes 16, 8, 0, 24
            const int shift = c == 0 ? 16 : c == 1 ? 8 : c == 2 ? 0 : 24;
            // the flattened mesh is a closed hull whose fans overlap a few layers deep, each multiplying: the console's shadow is
            // about the shade to the power kDarken where it is darkest; the map's mesh multiplies once, so it is given that
            static const float darken = std::getenv("PGR2_MAP_DARKEN") ? float(std::atof(std::getenv("PGR2_MAP_DARKEN"))) : 4.0f;
            const float want = std::clamp(std::pow(float(shade >> shift & 0xFF) / 255.0f, darken), 0.02f, 1.0f);
            const float ambient = F(car + kCarAmbient + c * 4);
            const float light = ambient * (1.0f / want - 1.0f) / lift;  // colour x lift = ambient (1 / shade - 1)
            colour |= uint32_t(std::clamp(std::lround(light * 255.0f), 0L, 255L)) << shift;
        }
        uint8_t sun[12];
        std::memcpy(sun, &kernel::G<uint8_t>(kSunDirection), 12);
        const uint32_t sun_colour = R32(kSunColour);
        std::memcpy(&kernel::G<uint8_t>(kSunDirection), dir, 12);
        kernel::W32(kSunColour, colour);
        pgr2::g_shadow_mark = pgr2::g_shadow_mark % 255 + 1;  // each light's shadow its own (draw_filter: one shadow a mark)
        const bool cpu = pgr2::cpu_car_shadows;
        pgr2::cpu_car_shadows = false;  // the mesh of this car alone (car_shadows.cpp would add the others')
        g_inner = true;
        ctx.ecx = kShadowObject;
        kernel::CallGuest(ctx, kDrawMap, {car, 0x3F800000u});
        ctx.ecx = car;
        kernel::CallGuest(ctx, kGroundMesh, {});
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
            LOGR("street light shadows: %u maps and meshes drawn in 2 s", pairs);
            pairs = 0;
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
        const float radius = std::min(F(ctx.esp + 12) * float(std::clamp<uint32_t>(pgr2::street_light_reach, 100, 300)) / 100.0f, 12.0f);
        WF(ctx.esp + 12, radius);
    }
    __imp__sub_000BF3F0(ctx, base);
}
