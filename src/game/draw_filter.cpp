// PGR2's draws the renderer changes (gpu::g_draw_filter)
#include <atomic>
#include <cstdlib>

#include <cstring>
#include <cstdio>
#include <set>
#include <vector>
#include <string>

#include "core/log.h"
#include "gpu/state.h"
#include "gpu/texture.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace pgr2 {
extern bool dlc, cpu_car_shadows;  // src/common/game_info.cpp
extern std::atomic<uint32_t> g_shadow_mark;  // src/game/car_shadows.cpp
void WaterDraw(const gpu::State& s, gpu::DrawAdjust& adjust);          // src/game/planar_water.cpp
void CarReflectionDraw(const gpu::State& s, gpu::DrawAdjust& adjust);  // src/game/car_reflections.cpp
}

namespace {

// The car's shadow: a ground mesh around the car that the game builds each frame, drawn after the scene with the car's
// shadow map (a depth texture compared in stage 0), a polygon offset, no depth writes and a multiply blend. The mesh
// does not lie on the visible road and reaches behind the camera; on D3D12 parts of it failed the depth test against
// the road, changing as the camera moved: the shadow and the sunlit road around the car flickered. Without the depth
// test it was steady but showed through the car. Measured headless (which shader's depth stopped each failed pixel,
// and by how much): the road and kerbs by at most 4096 units of a 24 bit buffer, the car body by 4096 to 65536. So
// the pass keeps its depth test, 4096 units nearer. 0x300 is GL_SRC_COLOR.
// That was measured with a Z buffer. Since W-buffering (the console's depth for PGR2's draws) 4096 units are about a
// quarter of a metre of w, and in the sun the shadow then reached onto the car's lower body (Harry, 2026-10-20); his
// runs without it showed the shadow right. So it is off unless PGR2_SHADOW_NEARER=1.
constexpr int32_t kCarShadowBias = -4096;

void CarShadowNearer(const gpu::State& s, gpu::DrawAdjust& adjust) {
    auto simple = [&](uint32_t method) { return s.simple[method >> 2]; };
    if (!s.rs[gpu::kRsZEnable] || simple(gpu::kMZWriteEnable) || !simple(gpu::kMSolidOffsetEnable)) return;
    if (!simple(gpu::kMBlendEnable) || simple(gpu::kMSrcBlend) != 0 || simple(gpu::kMDestBlend) != 0x300) return;
    gpu::TextureDesc t;
    if (!s.textures[0] || !gpu::Describe(s.textures[0], t) || !gpu::Format(t.format).depth) return;
    adjust.depth_bias = kCarShadowBias;
    // each vertex shader it applies to, once (a few: the pass is one shader)
    static std::atomic<uint32_t> seen[4]{};
    for (auto& h : seen) {
        uint32_t cur = h.load();
        if (cur == s.vs_handle) break;
        if (cur == 0 && h.compare_exchange_strong(cur, s.vs_handle)) {
            LOGR("pgr2: car shadow pass drawn %d depth units nearer (vertex shader %08X)", -kCarShadowBias, s.vs_handle);
            break;
        }
    }
}

// PGR2's error material: an object whose material the engine cannot set up is drawn with a pixel shader and its
// combiner constant 0 magenta (ARGB 0x00FF00FF). The DLC city packs' meshes in the title update's layout come out that
// way on the disc build, with garbage vertices too: huge magenta triangles over the sky and the HUD (Harry's captures,
// 2026-10-21: draws with pixel shader and c0 = 1 0 1 0, the later fixed function draws only inherit the constant)
constexpr uint32_t kErrorMagenta = 0x00FF00FF;
bool g_skip_error_material = false;

// PGR2_TRACE_SHADOW=1 (headless, drawing on the title's thread): the guest call chain of each kind of car shadow draw, once per
// different chain, for finding the routines that draw them
void TraceShadow(const gpu::State& s) {
    gpu::TextureDesc t;
    const bool have_tex = s.textures[0] && gpu::Describe(s.textures[0], t);
    const bool multiply = s.simple[gpu::kMBlendEnable >> 2] && s.simple[gpu::kMSrcBlend >> 2] == 0 && s.simple[gpu::kMDestBlend >> 2] == 0x300;
    const char* kind = nullptr;
    if ((!s.rt.header || !s.rt.data) && s.zs.header) kind = "depth map";
    else if (multiply && have_tex && gpu::Format(t.format).depth) kind = "mesh";
    else if (multiply && have_tex && t.width == 128 && t.height == 128 && t.format == 1) kind = "blob";
    if (!kind) return;
    kernel::Thread* th = kernel::Current();
    if (!th || !th->ctx) return;
    X86Context& c = *th->ctx;
    std::string chain = kind;
    char num[16];
    std::vector<uint32_t> found;
    for (uint32_t i = 0; i < 160; i++) {
        uint32_t a = c.esp + i * 4;
        if (!mem::IsCommitted(a)) break;
        uint32_t v = kernel::R32(a);
        if (v >= 0x11000 && v < 0x275000) {
            std::snprintf(num, sizeof num, " %X", v);
            chain += num;
        }
    }
    static std::set<std::string> seen;
    static int per_kind[3] = {};
    int& used = per_kind[kind[0] == 'd' ? 0 : kind[0] == 'm' ? 1 : 2];
    if (used < 8 && seen.insert(chain).second && ++used) LOGR("shadow chain: %s esi %08X edi %08X ebx %08X ebp %08X", chain.c_str(), c.esi, c.edi, c.ebx, c.ebp);
}
bool g_trace_shadow = false;

// With a sun shadow on every car (src/game/car_shadows.cpp) overlapping shadows would multiply the ground twice. PGR2 uses no stencil
// in a race, so each shadow draw leaves this frame's mark and skips pixels that have it. A shadow draw is a multiply reading a depth
// texture; the fragments its comparison leaves out are discarded (alpha kill on stage 0), so only shadowed pixels are marked. The
// marks are set for that one draw, here because the guest can't reach it alone, and the next draw's call puts the state back.
class OneShadowEach {
public:
    void Filter(const gpu::State& s) {
        if (set_) {
            gpu::State& state = const_cast<gpu::State&>(s);
            state.rs[gpu::kRsStencilEnable] = enable_;
            state.rs[gpu::kRsStencilFail] = fail_;
            for (int i = 0; i < kSimple; i++) state.simple[kMethods[i] >> 2] = saved_[i];
            set_ = false;
        }
        if (!pgr2::cpu_car_shadows || !s.simple[gpu::kMBlendEnable >> 2] || s.simple[gpu::kMSrcBlend >> 2] != 0 ||
            s.simple[gpu::kMDestBlend >> 2] != 0x300 || !s.textures[0])
            return;
        gpu::TextureDesc t;
        if (!gpu::Describe(s.textures[0], t) || !gpu::Format(t.format).depth) return;
        gpu::State& state = const_cast<gpu::State&>(s);
        enable_ = s.rs[gpu::kRsStencilEnable];
        fail_ = s.rs[gpu::kRsStencilFail];
        const uint32_t values[kSimple] = {kNotEqual, pgr2::g_shadow_mark, 0xFF, 0xFF, kKeep, kReplace};
        for (int i = 0; i < kSimple; i++) {
            saved_[i] = s.simple[kMethods[i] >> 2];
            state.simple[kMethods[i] >> 2] = values[i];
        }
        state.rs[gpu::kRsStencilEnable] = 1;
        state.rs[gpu::kRsStencilFail] = kKeep;
        set_ = true;
    }

private:
    static constexpr int kSimple = 6;
    static constexpr uint32_t kMethods[kSimple] = {gpu::kMStencilFunc, gpu::kMStencilRef, gpu::kMStencilMask,
                                                   gpu::kMStencilWriteMask, gpu::kMStencilZFail, gpu::kMStencilPass};
    static constexpr uint32_t kNotEqual = 0x205, kKeep = 0x1E00, kReplace = 0x1E01;
    bool set_ = false;
    uint32_t enable_ = 0, fail_ = 0, saved_[kSimple] = {};
};
OneShadowEach g_one_shadow;

void Filter(const gpu::State& s, gpu::DrawAdjust& adjust) {
    g_one_shadow.Filter(s);
    pgr2::WaterDraw(s, adjust);
    if (adjust.skip) return;
    pgr2::CarReflectionDraw(s, adjust);
    if (g_trace_shadow) TraceShadow(s);
    if (g_skip_error_material && s.ps && s.simple[gpu::kMPsConstant0 >> 2] == kErrorMagenta) {
        static std::atomic<int> said{0};
        if (said++ < 4) LOGR("pgr2: a draw with PGR2's error material (magenta) left out, vertex shader %08X", s.vs_handle);
        adjust.skip = true;
        return;
    }
    static const bool nearer = [] {
        const char* e = std::getenv("PGR2_SHADOW_NEARER");
        return e && *e == '1';
    }();
    if (nearer) CarShadowNearer(s, adjust);
}

}  // namespace

// called from the game's init (src/common/game_info.cpp): a static initializer here never ran in the Windows exe
void InstallDrawFilter() {
    const char* nearer = std::getenv("PGR2_SHADOW_NEARER");
    bool shadow = nearer && *nearer == '1';
    g_skip_error_material = pgr2::dlc;
    const char* trace = std::getenv("PGR2_TRACE_SHADOW");
    g_trace_shadow = trace && *trace == '1';
    gpu::g_draw_filter = Filter;
    if (shadow) LOGR("pgr2: PGR2_SHADOW_NEARER=1, draw filter installed (car shadow pass nearer)");
    if (g_skip_error_material) LOGR("pgr2: DLC cities on, draws with PGR2's error material are left out");
}
