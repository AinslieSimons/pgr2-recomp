// PGR2_LOD_PROBE=1: what the game's level of detail does during a race, a block of "lod:" log lines every 5 seconds. A
// measuring tool for the visuals work; with the variable unset each hook is one test and the original.
// Addresses and what they mean (found in the disassembly):
//   camera [0x3CD804]: +0x10 viewer (its +0x128 is the view mode), +0x14 mirrored, +0x18 far, +0x20 object range multiplier,
//     +0x30 viewport width, +0x40 kind: 0 main, 1 mirror, 2 reflection cube, 3 planar reflection, +0xE0 position, +0x130 scale
//   car: +0x44 position, +0xA4 radius, +0x15D0 detail level (written by 0x3215D for the camera being drawn)
//   texture block 0x3F2D38 + i * 0x18: +0 D3D header in use, +8 flags (0x80 streamed, 4 full version here, 2 being read),
//     +0x10 low byte the full version's size (log2, width and height nibbles); count 0x3F2D38 + 0xBB80
//   stream cache 0x28F5F0: +0x54 a small running count, not the bytes held (measured), +0x5C size, +0xD8 requests queued
//   block +0x14: low byte the full version's size in 4 KB pages
//   city draw object 0x436D90: instance buckets +0x47F0.. (caps 1000, 2000, -, 20) and +0x19C.. (cap 400)
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_00100810);
X86_EXTERN_FUNC(__imp__sub_001003E0);
X86_EXTERN_FUNC(__imp__sub_00100C00);
X86_EXTERN_FUNC(__imp__sub_00178110);
X86_EXTERN_FUNC(__imp__sub_00023020);
X86_EXTERN_FUNC(__imp__sub_00022AE0);
X86_EXTERN_FUNC(__imp__sub_00119430);
X86_EXTERN_FUNC(__imp__sub_001193E0);
X86_EXTERN_FUNC(__imp__sub_0011A670);

namespace {
using kernel::G;
using kernel::R32;

constexpr uint32_t kCamera = 0x3CD804, kBlocks = 0x3F2D38, kBlockStride = 0x18, kCache = 0x28F5F0, kCity = 0x436D90;
constexpr int kKinds = 4, kBinMetres = 25, kBins = 16, kLevels = 16;
const char* const kKindName[kKinds] = {"main", "mirror", "cube", "planar"};

bool On() {
    static const bool on = [] { const char* e = std::getenv("PGR2_LOD_PROBE"); return e && *e == '1'; }();
    return on;
}
float F(uint32_t va) {
    uint32_t v = R32(va);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
uint16_t U16(uint32_t va) { return G<uint16_t>(va); }
int Kind() {
    uint32_t cam = R32(kCamera);
    if (!cam) return 0;
    uint32_t k = R32(cam + 0x40);
    return k < kKinds ? int(k) : 0;
}

struct Window {
    uint64_t binds[kKinds], streamed[kKinds], low[kKinds], short_by[kKinds][8];
    uint64_t level[kKinds][kLevels], forced[kKinds], passes[kKinds];
    uint64_t bin_n[kKinds][kBins], bin_sum[kKinds][kBins];
    uint32_t bucket[kKinds][6];
    uint32_t distinct[kKinds], distinct_low[kKinds];
    uint64_t ring_setup, ring_frame, ring_tail;
} w;
std::array<std::vector<uint8_t>, kKinds> seen;  // per block: 1 bound, 2 bound while short of its full version
std::atomic<uint64_t> requests{0}, loaded{0}, evicted{0}, setup_walks{0};
uint32_t last_cam[kKinds];

void Flush() {
    uint32_t count = U16(kBlocks + 0xBB80), n_stream = 0, n_up = 0, n_have = 0, n_reading = 0, n_short = 0;
    uint64_t have_bytes = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t b = kBlocks + i * kBlockStride;
        uint8_t fl = G<uint8_t>(b + 8), dims = G<uint8_t>(b + 0x10);
        if (!(fl & 0x80)) continue;
        n_stream++;
        uint32_t hdr = R32(b);
        if (!hdr) continue;
        uint32_t f = R32(hdr + 0xC);
        int now = std::max<int>((f >> 20) & 0xF, (f >> 24) & 0xF), full = std::max<int>(dims >> 4, dims & 0xF);
        if (fl & 4) {
            n_have++;
            have_bytes += uint64_t(R32(b + 0x14) & 0xFF) << 12;
        }
        if (fl & 2) n_reading++;
        if (full > now) n_short++;
        if (full > now || (fl & 4)) n_up++;
    }
    LOGR("lod: attract demo 38384C=%u, player speed %g", G<uint8_t>(0x38384C), F(0x2973B0 + 0x19A0));
    LOGR("lod: flags 455CB6=%02X 28F6C9=%02X mirror option 366966=%u, distance gate in range %u / out of range %u", G<uint8_t>(0x455CB6),
         G<uint8_t>(0x28F6C9), G<uint8_t>(0x366966), R32(0x291ACC), R32(0x291AD0));
    LOGR("lod: stream cache holds %.2f MB of %.2f MB (+0x54 %u), queued %u; blocks %u, streamed %u, upgradable %u, full version here %u, reading %u, "
         "showing the small one %u", double(have_bytes) / 1048576.0, double(R32(kCache + 0x5C) & 0x7FFFFFFF) / 1048576.0, R32(kCache + 0x54), G<uint8_t>(kCache + 0xD8), count, n_stream, n_up,
         n_have, n_reading, n_short);
    LOGR("lod: window requests %llu, loads %llu, evictions %llu; setup walks %llu, request callers: setup %llu, per-frame %llu, other %llu",
         (unsigned long long)requests.exchange(0), (unsigned long long)loaded.exchange(0), (unsigned long long)evicted.exchange(0),
         (unsigned long long)setup_walks.load(), (unsigned long long)w.ring_setup, (unsigned long long)w.ring_frame,
         (unsigned long long)w.ring_tail);
    for (int k = 0; k < kKinds; k++) {
        if (!w.passes[k] && !w.binds[k]) continue;
        uint32_t cam = last_cam[k], viewer = cam ? R32(cam + 0x10) : 0;
        char lv[256] = "", bins[512] = "";
        size_t o = 0;
        for (int i = 0; i < kLevels; i++)
            if (w.level[k][i]) o += size_t(std::snprintf(lv + o, sizeof lv - o, " %d:%llu", i, (unsigned long long)w.level[k][i]));
        o = 0;
        for (int i = 0; i < kBins; i++)
            if (w.bin_n[k][i])
                o += size_t(std::snprintf(bins + o, sizeof bins - o, " %d-%dm:%.1f", i * kBinMetres, (i + 1) * kBinMetres,
                                          double(w.bin_sum[k][i]) / double(w.bin_n[k][i])));
        LOGR("lod: %s passes %llu view mode %d far %g range x%g factor %g width %u scale %g", kKindName[k], (unsigned long long)w.passes[k],
             viewer ? int(R32(viewer + 0x128)) : -1, cam ? F(cam + 0x18) : 0, cam ? F(cam + 0x20) : 0, cam ? F(cam + 0x24) : 0,
             cam ? R32(cam + 0x30) : 0, cam ? F(cam + 0x130) : 0);
        LOGR("lod: %s car levels%s, forced to 0: %llu; mean level by distance%s", kKindName[k], lv, (unsigned long long)w.forced[k], bins);
        LOGR("lod: %s binds %llu, streamed %llu, showing the small version %llu (%.1f%%), short by 1..4+ size steps %llu %llu %llu %llu; "
             "textures %u, %u of them small at least once", kKindName[k], (unsigned long long)w.binds[k], (unsigned long long)w.streamed[k],
             (unsigned long long)w.low[k], w.streamed[k] ? 100.0 * double(w.low[k]) / double(w.streamed[k]) : 0.0,
             (unsigned long long)w.short_by[k][1], (unsigned long long)w.short_by[k][2], (unsigned long long)w.short_by[k][3],
             (unsigned long long)w.short_by[k][4], w.distinct[k], w.distinct_low[k]);
        LOGR("lod: %s instance buckets at most %u/1000 %u/2000 %u %u/20, %u %u/400", kKindName[k], w.bucket[k][0], w.bucket[k][1], w.bucket[k][2],
             w.bucket[k][3], w.bucket[k][4], w.bucket[k][5]);
    }
    w = {};
    for (auto& s : seen) std::fill(s.begin(), s.end(), uint8_t(0));
}
}  // namespace

// a request for a block's full version
X86_FUNC(sub_00100810) {
    if (On()) requests++;
    __imp__sub_00100810(ctx, base);
}

// a block's full version read and in place
X86_FUNC(sub_001003E0) {
    if (On()) loaded++;
    __imp__sub_001003E0(ctx, base);
}

// ages a block in the cache and evicts it past 20
X86_FUNC(sub_00100C00) {
    if (!On()) return __imp__sub_00100C00(ctx, base);
    uint32_t flags = kBlocks + (R32(ctx.esp + 4) & 0xFFFF) * kBlockStride + 8;
    uint8_t before = G<uint8_t>(flags);
    __imp__sub_00100C00(ctx, base);
    if ((before & 4) && !(G<uint8_t>(flags) & 4)) evicted++;
}

// thiscall (stage, block): every material's texture bind
X86_FUNC(sub_00178110) {
    if (!On()) return __imp__sub_00178110(ctx, base);
    uint32_t i = R32(ctx.esp + 8), b = kBlocks + i * kBlockStride, hdr = R32(b);
    int k = Kind();
    if (hdr && i < 0x2000) {
        w.binds[k]++;
        if (seen[size_t(k)].empty()) seen[size_t(k)].resize(0x2000);
        uint8_t& s = seen[size_t(k)][i];
        if (!s) {
            s = 1;
            w.distinct[k]++;
        }
        if (G<uint8_t>(b + 8) & 0x80) {
            uint32_t f = R32(hdr + 0xC);
            uint8_t dims = G<uint8_t>(b + 0x10);
            int now = std::max<int>((f >> 20) & 0xF, (f >> 24) & 0xF), full = std::max<int>(dims >> 4, dims & 0xF);
            w.streamed[k]++;
            if (full > now) {
                w.low[k]++;
                w.short_by[k][std::min(full - now, 4)]++;
                if (!(s & 2)) {
                    s |= 2;
                    w.distinct_low[k]++;
                }
            }
        }
    }
    __imp__sub_00178110(ctx, base);
}

namespace pgr2 {
void ApplyCarDetail(uint32_t car);  // src/game/detail.cpp
}

// thiscall on the car: drawn at its detail level
X86_FUNC(sub_00023020) {
    pgr2::ApplyCarDetail(ctx.ecx);
    if (On()) {
        uint32_t car = ctx.ecx, cam = R32(kCamera);
        int k = Kind(), lv = G<uint8_t>(car + 0x15D0);
        w.level[k][std::min(lv, kLevels - 1)]++;
        if (cam) {
            float dx = F(car + 0x44) - F(cam + 0xE0), dy = F(car + 0x48) - F(cam + 0xE4), dz = F(car + 0x4C) - F(cam + 0xE8);
            int bin = std::min(int(std::sqrt(dx * dx + dy * dy + dz * dz)) / kBinMetres, kBins - 1);
            w.bin_n[k][bin]++;
            w.bin_sum[k][bin] += uint64_t(lv);
        }
    }
    __imp__sub_00023020(ctx, base);
}

// thiscall on the car: always level 0
X86_FUNC(sub_00022AE0) {
    if (On()) w.forced[Kind()]++;
    __imp__sub_00022AE0(ctx, base);
}

// the walk that asks for textures near a camera; one caller, race setup
X86_FUNC(sub_00119430) {
    if (On()) setup_walks++;
    __imp__sub_00119430(ctx, base);
}

// PGR2_TEX_DIST=metres (experiment): how far out a city cell asks for its textures' full versions. The per-frame call
// (0x11A49A) passes distance / 100 and 0x119380 asks when that is at most the texture's class, 0 for nearly all, so about 100 m,
// inside the gate at 0x11A45E (distance squared under the float 0x28F750, 500 m). Within the given distance this passes 0.
// PGR2_TEX_AFTER=seconds: only from that long after the first request, to compare both at one pose in one run.
int TexDist() {
    static const int d = [] {
        const char* e = std::getenv("PGR2_TEX_DIST");
        int v = e ? std::atoi(e) : 0;
        if (v > 0) LOGR("pgr2: PGR2_TEX_DIST=%d, full textures asked for out to that distance", v);
        return v;
    }();
    return d;
}

// a city cell asks for its textures, told apart by who returns to
X86_FUNC(sub_001193E0) {
    uint32_t ret = R32(ctx.esp);
    if (On()) {
        if (ret == 0x1194D0) w.ring_setup++;
        else if (ret == 0x11A49F) w.ring_frame++;
        else w.ring_tail++;
    }
    static const auto first = std::chrono::steady_clock::now();
    static const double after = std::getenv("PGR2_TEX_AFTER") ? std::atof(std::getenv("PGR2_TEX_AFTER")) : 0;
    bool wide = std::chrono::duration<double>(std::chrono::steady_clock::now() - first).count() >= after;
    if (int d = TexDist(); d > 0 && wide && ret == 0x11A49F) {
        float gate = std::max(float(d) * float(d), 250000.0f);
        std::memcpy(&G<uint32_t>(0x28F750), &gate, 4);
        if (int32_t(R32(ctx.esp + 4)) * 100 < d) kernel::W32(ctx.esp + 4, 0);
    }
    __imp__sub_001193E0(ctx, base);
}

// one camera's walk of the city
X86_FUNC(sub_0011A670) {
    __imp__sub_0011A670(ctx, base);
    if (!On()) return;
    int k = Kind();
    last_cam[k] = R32(kCamera);
    w.passes[k]++;
    const uint32_t off[6] = {0x47F0, 0x47F2, 0x47F4, 0x47F6, 0x19C, 0x19E};
    for (int i = 0; i < 6; i++) w.bucket[k][i] = std::max<uint32_t>(w.bucket[k][i], U16(kCity + off[i]));
    static auto next = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (k == 0 && now >= next) {
        next = now + std::chrono::seconds(5);
        Flush();
    }
}
