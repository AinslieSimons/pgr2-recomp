// Baked ambient occlusion for the city (gpu/baked_ao.h, Metal): which draws are the city's static meshes, and which city
// they belong to. Setting baked_ao, percent of the occlusion as baked (0 off).
#include "gpu/baked_ao.h"

#include <algorithm>
#include <mutex>
#include <unordered_set>

#include "core/log.h"
#include "gpu/shaders.h"
#include "gpu/state.h"
#include "kernel/kernel.h"

namespace pgr2 {
extern uint32_t baked_ao;

namespace {
constexpr uint32_t kCamera = 0x3CD804;  // the camera being drawn (src/game/draw_filter.cpp)
constexpr uint32_t kCity = 0x38317C;    // the race's city index

// the city's blocks: drawn by these vertex programs with their vertices in world coordinates and never changed (a census
// of the five demos' draws, 2026-11-09). Water (modes 0x64621) and blended draws left out
bool CityDraw(const gpu::State& s) {
    switch (s.vs_handle) {
        case 0xB1001B61: case 0xB1001B71: case 0xB1001BB1: case 0xB1001BE1: case 0xB1001B31:
        case 0xB1001C61: case 0xB1001CB1: case 0xB1001CE1: case 0xB1001CC1: break;
        default: return false;
    }
    if (s.rs[gpu::kRsPsTextureModes] == 0x64621 || s.simple[gpu::kMBlendEnable >> 2] || !kernel::R32(kCamera)) return false;
    if (s.vs) {
        static std::unordered_set<uint32_t> told;
        if (told.insert(s.vs_handle).second) LOGR("pgr2: baked ao: city program %08X, microcode hash %016llX", s.vs_handle, (unsigned long long)s.vs->hash);
    }
    gpu::ao::g_scene.store(kernel::R32(kCity) + 1, std::memory_order_relaxed);
    return true;
}
}  // namespace

void SetBakedAo(uint32_t percent) {
    baked_ao = std::min<uint32_t>(percent, 100);
    gpu::ao::g_strength.store(float(baked_ao) / 100.0f, std::memory_order_relaxed);
}

void InstallBakedAo() {
    gpu::ao::g_eligible = CityDraw;
    SetBakedAo(baked_ao);
}

}  // namespace pgr2
