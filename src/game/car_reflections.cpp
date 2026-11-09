// Every car reflects the street it is on (car_reflections, on by default). The followed car's paint, glass and lights
// sample a live reflection cube the game draws around it each frame (a 16 bit cube, D3DFMT R5G6B5 swizzled); every other
// car samples the city's FixedCubeMap, a DXT1 cube from the disc that never changes. Here a car draw sampling a DXT1
// cube takes the live one in its place (gpu::DrawAdjust::stage3). The cube is drawn at the followed car, so a car far
// from it reflects that car's street: far cars are small on screen, and a cut-off would make a car's reflection jump
// between the two cubes as it crossed it. FINDINGS.md, "Reflections".
//
// The cube itself draws no car (cars_in_reflections, on by default, adds them): its pass (0x180040) draws each face's
// world and flushes the frame's draw lists (0x186B20), but never asks the car manager (0x297390) to queue its cars, as
// the main view does (0x32010) just before its flush. So the flush, seeing the cube camera active, has the cars queued
// first, and the followed car, which the cube is drawn from inside, is left out by the cars' visibility check (0x1C900).
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "app/perf.h"
#include "core/log.h"
#include "game/perf_zones.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "gpu/shaders.h"
#include "gpu/state.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"

namespace pgr2 {

extern bool car_reflections, cars_in_reflections, traced_reflections;  // src/common/game_info.cpp
extern uint32_t reflection_far, reflection_range, traced_range, traced_ray;  // src/common/game_info.cpp
extern bool car_cubes;  // src/common/game_info.cpp
extern uint32_t car_cube_every;  // src/common/game_info.cpp
bool g_extra_cube = false;  // a car's own cube being drawn (src/game/hooks.cpp draws its every face)
bool CarCubeFor(const gpu::State& s, uint32_t& cube);  // below
bool InWaterReflection(const gpu::State& s);  // src/game/planar_water.cpp

namespace {
using kernel::R32;

// the pixel shaders' texture modes (kRsPsTextureModes) of car draws with a cube at stage 3, seen in races in three
// cities: paint 0x8401 (other cars) and 0x8421 (the followed car, some parts of others), glass and lights 0x8021,
// other parts 0x0421. World objects that sample the fixed cube use other modes (0x0001, 0x0021, 0x8001, water 0x64621).
bool CarModes(uint32_t modes) { return modes == 0x8401 || modes == 0x8421 || modes == 0x8021 || modes == 0x0421; }

constexpr uint32_t kLiveFormat = 0x05;   // R5G6B5, swizzled: the cube drawn each frame
constexpr uint32_t kFixedFormat = 0x0C;  // DXT1: FixedCubeMap

std::atomic<uint32_t> g_live{0};  // the live cube's texture, as the followed car's draws bind it

constexpr uint32_t kActiveCamera = 0x3CD804;  // the camera being drawn (the camera holder 0x3B59C0 + 0x17E44)
constexpr uint32_t kCamViewer = 0x10, kCamKind = 0x40, kKindCube = 2;
constexpr uint32_t kViewerMode = 0x128, kViewerCar = 0x140, kModeNoCar = 0x0C;  // view mode 0xC follows no car
constexpr uint32_t kCarManager = 0x297390, kQueueCars = 0x32010;
bool g_in_cube = false;   // the cube's cars being queued
uint32_t g_left_out = 0;  // the followed car then
uint32_t g_cube_car = 0;  // the car whose own cube is being drawn (car_cubes), left out of it

}  // namespace

// the draw filter's part (src/game/draw_filter.cpp)
void CarReflectionDraw(const gpu::State& s, gpu::DrawAdjust& adjust) {
    if (!car_reflections || !s.textures[3] || !CarModes(s.rs[gpu::kRsPsTextureModes])) return;
    gpu::TextureDesc t;
    if (!gpu::Describe(s.textures[3], t) || !t.cube) return;
    if (t.format == kLiveFormat) {
        g_live.store(s.textures[3], std::memory_order_relaxed);
        return;
    }
    if (t.format != kFixedFormat) return;
    // a car with a cube of its own (car_cubes) reads that
    uint32_t own = 0;
    if (CarCubeFor(s, own)) {
        adjust.stage3 = own;
        return;
    }
    const uint32_t live = g_live.load(std::memory_order_relaxed);
    gpu::TextureDesc l;
    if (!live || !gpu::Describe(live, l) || !l.cube || l.format != kLiveFormat) return;  // none yet, or gone with its race
    // drawn into it, not read from it: a target holds a physical address, a texture the virtual one (0x80000000 up)
    const uint32_t rt = s.rt.data & 0x0FFFFFFF, cube = l.data & 0x0FFFFFFF;
    if (rt >= cube && rt < cube + 6 * gpu::FaceSize(l)) return;
    adjust.stage3 = live;
    // once per cube (the game points a new header at the same cube from time to time; a race makes a new cube)
    static std::atomic<uint32_t> told{0};
    if (told.exchange(l.data) != l.data) LOGR("pgr2: other cars reflect the live cube (at %08X) in place of the city's (%08X)", l.data, t.data);
}


// Ray traced reflections (traced_reflections, Metal): every car draw's triangles go into the frame's scene, and the next
// frame's car paint and glass read their cube along a ray traced against it first, so cars reflect the cars around them
// and their own mirrors, spoilers and wheels. The car programs (vertex shaders reading c[110] to c[112] and c[126]) put
// the vertex in the world by c[110] to c[112], take the eye at c[126] and write the world's reflection direction to
// texture 3; c[114] to c[117] are the whole transform to the screen, scaled by c[58] and offset by c[59] after the divide
// by w. So a pixel's place in the world comes back from its screen position and w. The scene is the last frame's, so
// each pixel's place is carried into it by its own car's move since then (the same mesh drawn last frame nearest by).
namespace {

struct Affine {  // three rows of four: x y z 1 to x y z
    float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
};

Affine Rows(const gpu::State& s, int first) {
    Affine a;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) a.m[r * 4 + c] = s.vsc[size_t(first + r)][size_t(c)];
    return a;
}

Affine Mul(const Affine& a, const Affine& b) {  // a after b
    Affine o;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) {
            float v = c == 3 ? a.m[r * 4 + 3] : 0;
            for (int k = 0; k < 3; k++) v += a.m[r * 4 + k] * b.m[k * 4 + c];
            o.m[r * 4 + c] = v;
        }
    return o;
}

bool Invert(const Affine& a, Affine& o) {
    const float* m = a.m;
    const float c00 = m[5] * m[10] - m[6] * m[9], c01 = m[6] * m[8] - m[4] * m[10], c02 = m[4] * m[9] - m[5] * m[8];
    const float det = m[0] * c00 + m[1] * c01 + m[2] * c02;
    if (!(std::fabs(det) > 1e-12f)) return false;
    const float d = 1 / det;
    float* r = o.m;
    r[0] = c00 * d, r[1] = (m[2] * m[9] - m[1] * m[10]) * d, r[2] = (m[1] * m[6] - m[2] * m[5]) * d;
    r[4] = c01 * d, r[5] = (m[0] * m[10] - m[2] * m[8]) * d, r[6] = (m[2] * m[4] - m[0] * m[6]) * d;
    r[8] = c02 * d, r[9] = (m[1] * m[8] - m[0] * m[9]) * d, r[10] = (m[0] * m[5] - m[1] * m[4]) * d;
    for (int k = 0; k < 3; k++) r[k * 4 + 3] = -(r[k * 4] * m[3] + r[k * 4 + 1] * m[7] + r[k * 4 + 2] * m[11]);
    return true;
}

constexpr int kScreen = 114, kScale = 58, kOffset = 59;

// how a car program puts its vertex in the world: dp4 of v0 with c[rows] to c[rows + 2] (not the screen transform's
// c[114] to c[117]), rows -1 for none. Some programs' world is the absolute one and they take the eye from c[126] (other
// cars: c[110] to c[112]); the others' is centred on the eye already (the followed car: c[122] to c[124]). Both are in
// metres and turn as the world does. The screen transform is no help for the eye: a distant car is drawn as a smaller
// copy pulled toward the eye (into the depth range), by its c[114] to c[117] alone, which still map each pixel to its
// place on the mesh.
struct Program {
    int rows = -1;
    bool absolute = false;  // reads the eye, c[126]
};
constexpr int kEye = 126, kAbsoluteRows = 110, kEyeRows = 122;

Program ReadProgram(const gpu::VertexShader& vs) {
    Program p;
    uint64_t dp4 = 0;  // bit per constant 96 to 159
    for (size_t n = 0; n + 4 <= vs.program.size(); n += 4) {
        const uint32_t* t = &vs.program[n];
        auto f = [&](int sub, int bit, int len) { return (t[sub] >> bit) & ((1u << len) - 1); };
        const uint32_t mac = f(1, 21, 4), ilu = f(1, 25, 3), cr = f(1, 13, 8);
        if (mac == 7 && f(2, 26, 2) == 2 && f(1, 9, 4) == 0 && f(2, 11, 2) == 3 && !f(3, 1, 1) && cr >= 96 && cr < 160) dp4 |= 1ull << (cr - 96);
        const bool reads_c = (mac && (f(2, 26, 2) == 3 || f(2, 11, 2) == 3)) || ((mac || ilu) && f(3, 28, 2) == 3);
        if (reads_c && cr == kEye) p.absolute = true;
        if (f(3, 0, 1)) break;
    }
    for (int k = 96; k + 2 < 160 && p.rows < 0; k++)
        if ((k + 2 < kScreen || k > kScreen + 3) && (dp4 >> (k - 96) & 7) == 7) p.rows = k;
    // only the two layouts seen to be the world: c[110] with the eye at c[126], c[122] centred on the eye. Another program's
    // three rows may be something else (a glass program's c[118]), whose triangles put in the scene caught every ray
    if (!(p.rows == kAbsoluteRows && p.absolute) && !(p.rows == kEyeRows && !p.absolute)) p.rows = -1;
    return p;
}

}  // namespace
// the main view: drawn into a target the size of the screen (not a reflection cube's face), across it (not the mirror),
// and not the water's reflection, the scene drawn upside down below the water (whose cars put in the scene upside down
// caught every ray from the car above them)
bool MainView(const gpu::State& s) {
    const uint32_t w = (s.rt.size & 0xFFF) + 1;
    return s.rt.size && w >= 512 && s.viewport.w >= 512 && !InWaterReflection(s);
}
namespace {

struct Drawn {
    uint64_t mesh;
    Affine world;
};
std::vector<Drawn> g_this, g_last;  // the car draws of this frame and the last (the render thread's)
uint64_t g_frame = ~0ull;

// the same mesh frame to frame: its vertex buffer, count and first indices (their copy moves every frame), or its start
uint64_t MeshKey(const gpu::State& s, const gpu::DrawAdjust& a) {
    uint64_t at = a.start;
    if (a.indices) at = gpu::Hash(a.indices, std::min<size_t>(a.count, 32) * 2);
    return (uint64_t(s.streams[0].vb) << 32 ^ at * 0x9E3779B97F4A7C15ull) + a.count;
}

}  // namespace

void CarTraceDraw(const gpu::State& s, gpu::DrawAdjust& adjust) {
    if (!traced_reflections || !s.vs || !(s.vs_handle & 1) || !s.textures[3] || !CarModes(s.rs[gpu::kRsPsTextureModes])) return;
    static std::unordered_map<uint64_t, Program> programs;
    auto [it, fresh] = programs.try_emplace(s.vs->hash);
    if (fresh) {
        it->second = ReadProgram(*s.vs);
        if (it->second.rows >= 0)
            LOGR("pgr2: traced reflections: vertex shader %08X (modes %X) puts its vertex in the %s world by c[%d]", s.vs_handle,
                 s.rs[gpu::kRsPsTextureModes], it->second.absolute ? "absolute" : "eye's", it->second.rows);
        else LOGR("pgr2: traced reflections: vertex shader %08X (modes %X) has no world rows: left as it is", s.vs_handle, s.rs[gpu::kRsPsTextureModes]);
    }
    const Program& prog = it->second;
    if (prog.rows < 0 || !MainView(s)) return;
    if (adjust.frame != g_frame) {
        if (adjust.frame == g_frame + 1) g_last.swap(g_this);
        else g_last.clear();
        g_this.clear();
        g_frame = adjust.frame;
    }
    Affine world = Rows(s, prog.rows);
    // the pixel's place: x w, y w, w (console pixels) back through the screen transform's x, y and w rows to the object,
    // then into the world
    Affine screen, back;
    for (int c = 0; c < 4; c++) {
        screen.m[c] = s.vsc[kScreen][size_t(c)];
        screen.m[4 + c] = s.vsc[kScreen + 1][size_t(c)];
        screen.m[8 + c] = s.vsc[kScreen + 3][size_t(c)];
    }
    if (!Invert(screen, back)) return;
    const auto &k = s.vsc[kScale], &o = s.vsc[kOffset];
    if (k[0] == 0 || k[1] == 0) return;
    Affine unscale;  // (x w, y w, w) to the transform's x, y, w
    unscale.m[0] = 1 / k[0], unscale.m[2] = -o[0] / k[0];
    unscale.m[5] = 1 / k[1], unscale.m[6] = -o[1] / k[1];
    const Affine to_object = Mul(back, unscale);
    if (prog.absolute) {
        // the other cars' world rows leave out the model's size (their vertices are a tenth of a metre a unit), which only
        // their screen transform has: it puts the eye (w 0) a tenth as far from the car as c[126] is. That ratio is the size
        float d_eye = 0, d_screen = 0;
        for (int r = 0; r < 3; r++) {
            const float t = world.m[r * 4 + 3];
            const float e = world.m[r * 4] * to_object.m[3] + world.m[r * 4 + 1] * to_object.m[7] + world.m[r * 4 + 2] * to_object.m[11] + t;
            d_eye += (t - s.vsc[kEye][size_t(r)]) * (t - s.vsc[kEye][size_t(r)]);
            d_screen += (t - e) * (t - e);
        }
        if (!(d_screen > 1e-6f)) return;
        const float size = std::sqrt(d_eye / d_screen);
        static bool told = false;
        if (!told) told = true, LOGR("pgr2: traced reflections: other cars' models are %.3f times their world rows", size);
        // the scene is centred on the eye: frames apart, only each car's place relative to it matters
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) world.m[r * 4 + c] *= size;
            world.m[r * 4 + 3] -= s.vsc[kEye][size_t(r)];
        }
    }
    // one frame for every car: the followed car's programs put it in a world turned with the camera, the others' in the
    // city's own, so each is taken into the camera's: its own frame turned about the eye by the rotation the frame's
    // projection (screen after world, back) shows. That projection is the same lens for every car in the view, so the
    // rotation, read off its w row, then its y and x rows (each less what the rows below already give), is each frame's own
    float turn_cam[9];
    {
        Affine inv_world;
        if (!Invert(world, inv_world)) return;
        const Affine lens = Mul(screen, inv_world);
        const Affine eye_of = Mul(world, to_object);
        float rows[3][3];
        for (int r = 2; r >= 0; r--) {
            float v[3] = {lens.m[r * 4], lens.m[r * 4 + 1], lens.m[r * 4 + 2]};
            for (int q = r + 1; q < 3; q++) {
                const float d = v[0] * rows[q][0] + v[1] * rows[q][1] + v[2] * rows[q][2];
                for (int c = 0; c < 3; c++) v[c] -= d * rows[q][c];
            }
            const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (!(n > 0)) return;
            for (int c = 0; c < 3; c++) rows[r][c] = v[c] / n;
        }
        Affine cam;
        for (int r = 0; r < 3; r++) {
            float t = 0;
            for (int c = 0; c < 3; c++) cam.m[r * 4 + c] = turn_cam[r * 3 + c] = rows[r][c], t += rows[r][c] * eye_of.m[c * 4 + 3];
            cam.m[r * 4 + 3] = -t;
        }
        world = Mul(cam, world);
    }
    Affine from = Mul(world, to_object);
    // PGR2_TRACE_CHECK=frame: a few vertices of each car draw that frame through the game's own projection and back
    static const uint64_t check = std::getenv("PGR2_TRACE_CHECK") ? std::strtoull(std::getenv("PGR2_TRACE_CHECK"), nullptr, 10) : 0;
    if (check && adjust.frame == check) {
        std::vector<float> tri;
        gpu::DrawTriangles(s, 5, adjust.start, adjust.count - adjust.count % 3, adjust.indices, 0, tri);
        for (size_t i = 0; i + 2 < tri.size() && i < 9; i += 3) {
            const float v[4] = {tri[i], tri[i + 1], tri[i + 2], 1};
            auto dot = [&](int r) { return v[0] * s.vsc[size_t(r)][0] + v[1] * s.vsc[size_t(r)][1] + v[2] * s.vsc[size_t(r)][2] + s.vsc[size_t(r)][3]; };
            const float X = dot(kScreen), Y = dot(kScreen + 1), W = dot(kScreen + 3);
            const float sx = X * k[0] / W + o[0], sy = Y * k[1] / W + o[1];
            const float u[3] = {sx * W, sy * W, W};
            float got[3], want[3];
            for (int r = 0; r < 3; r++) {
                got[r] = from.m[r * 4] * u[0] + from.m[r * 4 + 1] * u[1] + from.m[r * 4 + 2] * u[2] + from.m[r * 4 + 3];
                want[r] = world.m[r * 4] * v[0] + world.m[r * 4 + 1] * v[1] + world.m[r * 4 + 2] * v[2] + world.m[r * 4 + 3];
            }
            LOGR("trace check: vs %08X v %.3f %.3f %.3f screen %.1f %.1f w %.1f: back %.3f %.3f %.3f, world %.3f %.3f %.3f", s.vs_handle,
                 v[0], v[1], v[2], sx, sy, W, got[0], got[1], got[2], want[0], want[1], want[2]);
        }
    }
    const uint64_t mesh = MeshKey(s, adjust);
    // into the scene once a frame
    bool again = false;
    for (const Drawn& d : g_this) again |= d.mesh == mesh && !std::memcmp(d.world.m, world.m, sizeof(world.m));
    // a car far from the eye left out (traced_range, 400 m by default: the demos' and replays' long lenses watch a pack
    // 100 m off and more)
    const float reach = float(std::clamp<uint32_t>(traced_range, 20, 5000));
    const bool near = world.m[3] * world.m[3] + world.m[7] * world.m[7] + world.m[11] * world.m[11] < reach * reach;
    if (!again && near) {
        adjust.trace_scene = true;
        adjust.trace_draw = uint32_t(g_this.size());
        adjust.trace_mesh = mesh;
        g_this.push_back({mesh, world});
        std::memcpy(adjust.trace_world, world.m, sizeof(world.m));
        // as the car's own programs shade it: stage 0's texture at v2 times c[163].y over c[163].w, times the paint's colour
        // (c[147]) lit, plus the cube (scaled by the texture's alpha); glass dark and shinier
        const bool glass = s.rs[gpu::kRsPsTextureModes] == 0x8021;
        adjust.trace_glass = glass;
        const auto& uv = s.vsc[163];
        adjust.trace_uv = uv[3] != 0 ? uv[1] / uv[3] : 0;
        for (int c = 0; c < 3; c++) adjust.trace_colour[c] = glass ? 0.08f : std::clamp(s.vsc[147][size_t(c)], 0.0f, 1.0f);
        adjust.trace_colour[3] = glass ? 0.7f : 0.6f;
        const auto& l = s.vsc[prog.absolute ? 128 : 127];  // the sun, as each program lights its colour
        const float n = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
        for (int c = 0; c < 3; c++)
            adjust.trace_light[c] = n > 0 ? -(turn_cam[c * 3] * l[0] + turn_cam[c * 3 + 1] * l[1] + turn_cam[c * 3 + 2] * l[2]) / n : 0;
        adjust.trace_light[3] = 0.35f;
    }
    // carried into the last frame's scene by this car's move since: the same mesh then, nearest
    const Drawn* was = nullptr;
    float best = 25.0f;  // 5 metres
    for (const Drawn& d : g_last) {
        if (d.mesh != mesh) continue;
        const float dx = d.world.m[3] - world.m[3], dy = d.world.m[7] - world.m[7], dz = d.world.m[11] - world.m[11];
        const float dd = dx * dx + dy * dy + dz * dz;
        if (dd < best) best = dd, was = &d;
    }
    if (check && adjust.frame == check)
        LOGR("trace match: vs %08X mesh %016llX again %d, last frame %zu draws (frame %llu), this %zu, found %d", s.vs_handle,
             (unsigned long long)mesh, int(again), g_last.size(), (unsigned long long)g_frame, g_this.size(), int(was != nullptr));
    Affine turn, inv;
    if (was && Invert(world, inv)) {
        const Affine move = Mul(was->world, inv);
        from = Mul(move, from);
        turn = move;
        adjust.trace_self = uint32_t(was - g_last.data()) + 1;
    }
    adjust.trace_reflect = true;
    std::memcpy(adjust.trace_from, from.m, sizeof(from.m));
    std::memcpy(adjust.trace_turn, turn.m, sizeof(turn.m));
    adjust.trace_range[0] = float(std::clamp<uint32_t>(traced_ray, 5, 1000));  // metres (traced_ray, 60 by default)
    adjust.trace_range[1] = 0.05f;
}

// the frame's draw lists flushed (thiscall, the camera): for the cube camera, the cars other than the followed one queued
// first, as the main view does
X86_EXTERN_FUNC(__imp__sub_00186B20);
X86_FUNC(sub_00186B20) {
    const uint32_t active = R32(kActiveCamera);
    if (pgr2::cars_in_reflections && !g_in_cube && active && R32(active + kCamKind) == kKindCube) {
        const uint32_t camera = ctx.ecx, viewer = R32(active + kCamViewer);
        g_left_out = g_cube_car ? g_cube_car : viewer && R32(viewer + kViewerMode) != kModeNoCar ? R32(viewer + kViewerCar) : 0;
        g_in_cube = true;
        ctx.ecx = kCarManager;
        kernel::CallGuest(ctx, kQueueCars, {});
        g_in_cube = false;
        ctx.ecx = camera;
    }
    perf::Zone zone(pgr2::kZFlush);
    __imp__sub_00186B20(ctx, base);
}

// the performance recorder's view of two functions nothing else overrides
X86_EXTERN_FUNC(__imp__sub_00117C40);
X86_FUNC(sub_00117C40) {
    perf::Zone zone(pgr2::kZWorldDraw);
    __imp__sub_00117C40(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_00032010);
X86_FUNC(sub_00032010) {
    perf::Zone zone(pgr2::kZQueueCars);
    __imp__sub_00032010(ctx, base);
}

// A cube of its own for every other car (car_cubes). The game makes one cube object (0x4862D0, 0xB4 bytes: six face
// headers, the faces at +0x78, the cube texture at +0xA8, the depth surface at +0xAC) by 0x1788F0 (thiscall: depth,
// size, format; ret 0xC) at a race's start and frees it by 0x1786C0. One more is made and freed with it for each car
// slot. After the game's cube pass, the pass is run again for each other car with the cube camera's cube (+0xBEF4) and
// place (+0xBEF8, the car's +0x44 raised as the game raises the followed car's) moved to that car, which is left out of
// it, the followed car put in. A car's draws (found by their world rows' place, other cars' c[110] to c[112], which is
// the car's +0x44) then read its own cube. Each costs about what the game's own cube does, so they take turns
// (car_cube_every: each is redrawn one frame in that many); every car's place is kept current each frame all the same,
// as its draws are matched by it.
namespace {
constexpr uint32_t kGameCube = 0x4862D0, kMakeCube = 0x1788F0, kFreeCube = 0x1786C0, kCubeTexture = 0xA8, kCubeSize = 0xC0;
constexpr uint32_t kCamCube = 0xBEF4, kCamAt = 0xBEF8, kCubeRaise = 0x275F58;
constexpr uint32_t kCars = 0x297390, kCarCount = 0x0C, kCarFirst = 0x1C00, kCarStride = 0x6990, kCarAt = 0x44;
constexpr uint32_t kSlots = 8;  // the car list's slots (a race has up to 8 cars)

struct CarCube {
    uint32_t object = 0;              // in guest memory (0 not yet)
    std::atomic<uint32_t> texture{0};  // its cube texture once drawn this race, 0 none
    std::atomic<uint32_t> at[3];       // the car's place (float bits) when it was drawn
};
CarCube g_cubes[kSlots];

float GetF(uint32_t va) { float f; std::memcpy(&f, &kernel::G<uint32_t>(va), 4); return f; }
void PutF(uint32_t va, float f) { std::memcpy(&kernel::G<uint32_t>(va), &f, 4); }

void DrawCarCubes(X86Context& ctx, uint32_t cam) {
    const uint32_t viewer = R32(cam + kCamViewer);
    const uint32_t followed = viewer && R32(viewer + kViewerMode) != kModeNoCar ? R32(viewer + kViewerCar) : 0;
    if (!followed) return;
    uint32_t saved[4];
    for (int i = 0; i < 4; i++) saved[i] = R32(cam + kCamCube + i * 4);
    const uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx;
    const uint32_t count = std::min<uint32_t>(R32(kCars + kCarCount), kSlots);
    static uint32_t turn = 0;
    turn++;
    const uint32_t every = std::clamp<uint32_t>(car_cube_every, 1, 8);
    for (uint32_t k = 0; k < count; k++) {
        CarCube& c = g_cubes[k];
        const uint32_t car = kCars + kCarFirst + k * kCarStride;
        if (car == followed || !c.object || !R32(c.object + kCubeTexture)) continue;
        for (int i = 0; i < 3; i++) c.at[i].store(R32(car + kCarAt + i * 4), std::memory_order_relaxed);
        if ((k + turn) % every != 0) continue;
        kernel::W32(cam + kCamCube, c.object);
        for (int i = 0; i < 3; i++) PutF(cam + kCamAt + i * 4, GetF(car + kCarAt + i * 4) + (i == 1 ? GetF(kCubeRaise) : 0.0f));
        g_cube_car = car;
        g_extra_cube = true;
        ctx.ecx = cam;
        kernel::CallGuest(ctx, 0x180040, {});
        g_extra_cube = false;
        g_cube_car = 0;
        c.texture.store(R32(c.object + kCubeTexture), std::memory_order_release);
    }
    ctx.eax = eax, ctx.ecx = ecx, ctx.edx = edx;
    for (int i = 0; i < 4; i++) kernel::W32(cam + kCamCube + i * 4, saved[i]);
    static bool said = false;
    if (!said) said = true, LOGR("pgr2: car cubes: a cube drawn at each of %u other cars", count ? count - 1 : 0);
}
}  // namespace

// the draw filter's part: the cube of the car this draw is of (its world rows' place within a few metres of the car's)
bool CarCubeFor(const gpu::State& s, uint32_t& cube) {
    if (!car_cubes || !s.vs) return false;
    float best = 4.0f * 4.0f;
    cube = 0;
    for (CarCube& c : g_cubes) {
        const uint32_t tex = c.texture.load(std::memory_order_acquire);
        if (!tex) continue;
        float d = 0;
        for (int r = 0; r < 3; r++) {
            const uint32_t bits = c.at[r].load(std::memory_order_relaxed);
            float at;
            std::memcpy(&at, &bits, 4);
            const float t = s.vsc[kAbsoluteRows + r][3] - at;
            d += t * t;
        }
        if (d < best) best = d, cube = tex;
    }
    return cube != 0;
}

// the game's cube made at a race's start: one for each car slot too, the same
X86_EXTERN_FUNC(__imp__sub_001788F0);
X86_FUNC(sub_001788F0) {
    const uint32_t self = ctx.ecx, depth = R32(ctx.esp + 4), size = R32(ctx.esp + 8), format = R32(ctx.esp + 12);
    __imp__sub_001788F0(ctx, base);
    if (self != kGameCube || !car_cubes) return;
    const uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx;
    for (CarCube& c : g_cubes) {
        if (!c.object) {
            c.object = mem::AllocSystem(kCubeSize);
            std::memset(&kernel::G<uint8_t>(c.object), 0, kCubeSize);
        }
        c.texture.store(0);
        if (R32(c.object + kCubeTexture)) {
            ctx.ecx = c.object;
            kernel::CallGuest(ctx, kFreeCube, {});
        }
        ctx.ecx = c.object;
        kernel::CallGuest(ctx, kMakeCube, {depth, size, format});
    }
    ctx.eax = eax, ctx.ecx = ecx, ctx.edx = edx;
    LOGR("pgr2: car cubes: %u more cubes made (%ux%u, format %u)", kSlots, size, size, format);
}

// the game's cube freed: the cars' too
X86_EXTERN_FUNC(__imp__sub_001786C0);
X86_FUNC(sub_001786C0) {
    const uint32_t self = ctx.ecx;
    __imp__sub_001786C0(ctx, base);
    if (self != kGameCube) return;
    const uint32_t eax = ctx.eax, ecx = ctx.ecx;
    for (CarCube& c : g_cubes) {
        c.texture.store(0);
        if (!c.object || !R32(c.object + kCubeTexture)) continue;
        ctx.ecx = c.object;
        kernel::CallGuest(ctx, kFreeCube, {});
    }
    ctx.eax = eax, ctx.ecx = ecx;
}

// The cube pass (thiscall, the cube camera): its far plane (camera +0x18, 30 m) and object range (+0x20, a tenth of each
// object's own) as the reflection settings ask. The main view's are 3000 m (the race far 0x49DFDC) and the whole range
// (+0x24). reflection_far: metres, 0 the console's; reflection_range: 0 the console's, 1 three times it (the mirror's
// share), 2 ten times (the main view's).
X86_EXTERN_FUNC(__imp__sub_00180040);
X86_FUNC(sub_00180040) {
    constexpr uint32_t kCamFar = 0x18, kCamRange = 0x20;
    static float console_far = 0, console_range = 0, wrote_far = -1, wrote_range = -1;
    const uint32_t cam = ctx.ecx;
    if (cam && R32(cam + kCamKind) == kKindCube) {
        auto get = [](uint32_t va) { float f; std::memcpy(&f, &kernel::G<uint32_t>(va), 4); return f; };
        auto put = [](uint32_t va, float f) { std::memcpy(&kernel::G<uint32_t>(va), &f, 4); };
        const float far = get(cam + kCamFar), range = get(cam + kCamRange);
        if (far != wrote_far) console_far = far;  // the game's own (set again by the game, or never written by us)
        if (range != wrote_range) console_range = range;
        static bool said = false;
        if (!said) LOGR("pgr2: reflection cube far %g, object range %g (the console's)", console_far, console_range), said = true;
        wrote_far = reflection_far ? float(reflection_far) : console_far;
        static const float kRangeTimes[] = {1, 3, 10};
        wrote_range = console_range * kRangeTimes[std::min<uint32_t>(reflection_range, 2)];
        put(cam + kCamFar, wrote_far);
        put(cam + kCamRange, wrote_range);
    }
    {
        perf::Zone zone(g_extra_cube ? -1 : int(pgr2::kZCubePass));
        __imp__sub_00180040(ctx, base);  // its ret has popped the return address
    }
    // then every other car's own cube, by the same pass (g_extra_cube: straight to the game's)
    if (!g_extra_cube && car_cubes && cam && R32(cam + kCamKind) == kKindCube) {
        perf::Zone zone(pgr2::kZCarCubes);
        DrawCarCubes(ctx, cam);
    }
}

// whether a car is drawn (thiscall, the car; al): not the followed car while the cube's cars are queued
X86_EXTERN_FUNC(__imp__sub_0001C900);
X86_FUNC(sub_0001C900) {
    if (g_in_cube && ctx.ecx == g_left_out) {
        ctx.eax = 0;
        ctx.esp += 4;  // ret
        return;
    }
    __imp__sub_0001C900(ctx, base);
}

}  // namespace pgr2
