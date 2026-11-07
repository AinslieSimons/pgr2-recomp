// Every car reflects the street it is on (car_reflections, on by default). The followed car's paint, glass and lights
// sample a live reflection cube the game draws around it each frame (a 16 bit cube, D3DFMT R5G6B5 swizzled); every other
// car samples the city's FixedCubeMap, a DXT1 cube from the disc that never changes. Here a car draw sampling a DXT1
// cube takes the live one in its place (gpu::DrawAdjust::stage3). The cube is drawn at the followed car, so a car far
// from it reflects that car's street: far cars are small on screen, and a cut-off would make a car's reflection jump
// between the two cubes as it crossed it. FINDINGS.md, "Reflections".
#include <atomic>
#include <cstdint>

#include "core/log.h"
#include "gpu/state.h"
#include "gpu/texture.h"

namespace pgr2 {

extern bool car_reflections;  // src/common/game_info.cpp

namespace {

// the pixel shaders' texture modes (kRsPsTextureModes) of car draws with a cube at stage 3, seen in races in three
// cities: paint 0x8401 (other cars) and 0x8421 (the followed car, some parts of others), glass and lights 0x8021,
// other parts 0x0421. World objects that sample the fixed cube use other modes (0x0001, 0x0021, 0x8001, water 0x64621).
bool CarModes(uint32_t modes) { return modes == 0x8401 || modes == 0x8421 || modes == 0x8021 || modes == 0x0421; }

constexpr uint32_t kLiveFormat = 0x05;   // R5G6B5, swizzled: the cube drawn each frame
constexpr uint32_t kFixedFormat = 0x0C;  // DXT1: FixedCubeMap

std::atomic<uint32_t> g_live{0};  // the live cube's texture, as the followed car's draws bind it

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
    const uint32_t live = g_live.load(std::memory_order_relaxed);
    gpu::TextureDesc l;
    if (!live || !gpu::Describe(live, l) || !l.cube || l.format != kLiveFormat) return;  // none yet, or gone with its race
    adjust.stage3 = live;
    static std::atomic<uint32_t> told{0};
    if (told.exchange(live) != live) LOGR("pgr2: other cars reflect the live cube (texture %08X) in place of the city's (%08X)", live, s.textures[3]);
}

}  // namespace pgr2
