// Every streamed texture at its full size, all the time. The numbers are in FINDINGS.md, "Textures".
// The console holds a small version of most city textures and streams the full one into the cache 0x28F5F0: a contiguous block
// of a few MB with 250 slots, for what is within about 100 m (0x119380). A full version not asked for in a frame goes back to its
// small header at once (0x100C00 swaps block +0 and +4) and leaves the cache 20 frames later. A city's full versions come to as
// much as 55 MB (Chicago), which the console's 64 MB has no room for. Here they all load with the city into guest memory past
// what the title allocates, addressed directly: the renderers take a texture's data as a virtual address when it is past the
// physical range (mem::VirtualFromPhysical). A block held here has its header at +0, the small one kept at +4, and flag 4 set,
// which already stops requests (0x100810 and 0x1078F0 test flags & 7); the hooks below stop ageing and swapping.
// Blocks (0x3F2D38 + i * 0x18): +0 header in use, +4 the other, +8 flags (0x80 compressed in the stream file, 8 no mipmaps, 4 full
// version here, 2 being read), +9 format, +0xC cache slot, +0x10 low byte log2 width and height, above it the compressed size
// << 8, +0x14 low byte the full size in pages, above it the file position in 512 byte units << 8.
// The stream file is only read synchronously between 0x100AC0(0) and 0x100AC0(1); the game loads what is near inside that span
// with 0x1078F0(0), which is where this runs, so what is read here needs no thread of its own.
// PGR2_FULL_TEXTURES=0 turns it off without a settings file.
#include <bitset>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

X86_EXTERN_FUNC(__imp__sub_00100990);
X86_EXTERN_FUNC(__imp__sub_00100CF0);
X86_EXTERN_FUNC(__imp__sub_00100550);
X86_EXTERN_FUNC(__imp__sub_001078F0);
X86_EXTERN_FUNC(__imp__sub_00107260);
X86_EXTERN_FUNC(__imp__sub_001072D0);

namespace pgr2 {
extern bool full_textures;

namespace {
using kernel::G;
using kernel::R32;
using kernel::W32;

constexpr uint32_t kBlocks = 0x3F2D38, kBlockStride = 0x18, kBlockCount = 0xBB80;
constexpr uint32_t kCache = 0x28F5F0, kCacheFileBase = 0x60, kCacheAges = 0x3C, kCacheReady = 0xD9;
constexpr uint32_t kFile = 0x28F660, kFileMemory = 0x60, kFileRead = 0x64, kFileHandle = 0x54;
constexpr uint32_t kLevels = 0x106FB0, kSetHeader = 0x1E0645, kSeek = 0x18C041, kRead = 0x1361B0, kInflate = 0x136E70, kEvict = 0x100C00;
constexpr uint8_t kCompressed = 0x80, kHere = 4, kReading = 2, kNoMips = 8;
constexpr uint32_t kHeaderBytes = 0x14, kPage = 0x1000, kInflateState = 0x38;

// the guest's user allocator starts at 64 KB and the title takes tens of MB of it, so this is never reached
constexpr uint32_t kArena = 0x40000000, kArenaSize = 0x30000000;
uint32_t g_used = 0, g_committed = 0;
std::bitset<0x2000> g_stream;  // blocks with a full version in the stream file: only 0x1072D0 writes their sizes, others keep stale ones

bool Held(uint32_t block) {
    uint32_t header = R32(block);
    return header >= kArena && header < kArena + kArenaSize;
}

uint32_t Take(uint32_t bytes, uint32_t align) {
    uint32_t at = (g_used + align - 1) & ~(align - 1);
    if (uint64_t(at) + bytes > kArenaSize) return 0;
    if (at + bytes > g_committed) {
        uint32_t want = (at + bytes + 0xFFFFF) & ~0xFFFFFu;
        if (!mem::Commit(kArena + g_committed, want - g_committed)) return 0;
        g_committed = want;
    }
    g_used = at + bytes;
    return kArena + at;
}

// the game's own copy in the cache goes, so that its slot and pages are free for anything else; false if the GPU is using it
bool Release(X86Context& ctx, uint32_t i, uint32_t block) {
    uint8_t& age = G<uint8_t>(R32(kCache + kCacheAges) + G<uint8_t>(block + 0xC));
    if (age == 0) {  // +0 full, +4 small: 0x100C00 expects them the other way once a block has aged
        uint32_t full = R32(block);
        W32(block, R32(block + 4));
        W32(block + 4, full);
    }
    age = 0x14;
    ctx.ecx = kCache;
    return kernel::CallGuest(ctx, kEvict, {i}) & 0xFF;
}

// as 0x100550 and 0x1003E0 do for one block, into memory of its own
bool Load(X86Context& ctx, uint32_t block, uint64_t& packed_total) {
    const uint32_t sizes = R32(block + 0x10), place = R32(block + 0x14);
    const uint32_t full = (place & 0xFF) << 12, packed = sizes >> 8;
    const uint32_t padded = (packed + 0x1FF) & ~0x1FFu;
    // the padding comes first, so that the packed bytes end where the texture ends and it can inflate over them
    const uint32_t before = padded > full ? (padded - full + kPage - 1) & ~(kPage - 1) : 0;
    const uint32_t mark = g_used;
    const uint32_t header = Take(kHeaderBytes, 4), memory = header ? Take(before + full, kPage) : 0;
    if (!memory) {
        g_used = mark;
        return false;
    }
    const uint32_t data = memory + before;
    const uint32_t w = 1u << ((sizes >> 4) & 0xF), h = 1u << (sizes & 0xF);
    const uint32_t levels = kernel::CallGuest(ctx, kLevels, {w, h, (G<uint8_t>(block + 8) & kNoMips) ? 1u : 0u});
    kernel::CallGuest(ctx, kSetHeader, {w, h, levels, 0, G<uint8_t>(block + 9), 0, header, 0, 0});
    W32(header + 4, data);

    const uint32_t from = (place >> 8) * 512 + R32(kCache + kCacheFileBase), in_memory = R32(kFile + kFileMemory);
    if (in_memory) W32(kFile + kFileRead, in_memory + from);
    else kernel::CallGuest(ctx, kSeek, {R32(kFile + kFileHandle), from, 0, 0});
    ctx.ecx = kFile;
    kernel::CallGuest(ctx, kRead, {data + full - padded, padded, from});
    ctx.esp -= kInflateState;  // its state is the caller's local
    ctx.ecx = ctx.esp;
    kernel::CallGuest(ctx, kInflate, {data + full - packed, packed, data, full});
    ctx.esp += kInflateState;

    W32(block, header);
    G<uint8_t>(block + 8) = uint8_t((G<uint8_t>(block + 8) & ~kReading) | kHere);
    packed_total += packed;
    return true;
}

void LoadAll(X86Context& ctx) {
    if (!full_textures || !(G<uint8_t>(kCache + kCacheReady) & 2)) return;
    const auto start = std::chrono::steady_clock::now();
    const uint32_t count = G<uint16_t>(kBlocks + kBlockCount), used_before = g_used;
    uint32_t loaded = 0, busy = 0, reading = 0, failed = 0;
    uint64_t packed = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t block = kBlocks + i * kBlockStride;
        const uint8_t flags = G<uint8_t>(block + 8);
        if (i >= g_stream.size() || !g_stream[i] || !(flags & kCompressed) || !R32(block) || Held(block)) continue;
        if (flags & kReading) {
            reading++;
            continue;
        }
        if ((flags & kHere) && !Release(ctx, i, block)) {
            busy++;
            continue;
        }
        if (Load(ctx, block, packed)) loaded++;
        else failed++;
    }
    if (!loaded && !busy && !reading && !failed) return;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    LOGR("full textures: %u loaded, %.1f MB from %.1f MB packed in %.2f s, %.1f MB held; left to the game: %u in use, %u being read, %u failed", loaded,
         double(g_used - used_before) / 1048576.0, double(packed) / 1048576.0, seconds, double(g_used) / 1048576.0, busy, reading, failed);
}

// a new city: what is held goes back to its small version, and the memory is used again (kept committed, as a stale bind then
// shows the wrong picture for a moment rather than stopping the game)
void ForgetAll() {
    const uint32_t count = G<uint16_t>(kBlocks + kBlockCount);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t block = kBlocks + i * kBlockStride;
        if (!Held(block)) continue;
        W32(block, R32(block + 4));
        G<uint8_t>(block + 8) = uint8_t(G<uint8_t>(block + 8) & ~kHere);
    }
    g_used = 0;
}
}  // namespace

void MarkBlocks(uint32_t first, bool stream) {
    const uint32_t count = G<uint16_t>(kBlocks + kBlockCount);
    for (uint32_t i = first; i < count && i < g_stream.size(); i++) {
        const uint32_t block = kBlocks + i * kBlockStride;
        // a texture with no full version has nothing or leftover bytes in these: a compressed size past its full size gives it away
        const uint32_t full = (R32(block + 0x14) & 0xFF) << 12, packed = R32(block + 0x10) >> 8;
        g_stream[i] = stream && full && packed && packed <= full + 0x200;
    }
}

bool FullTextureHeld(uint32_t i) { return Held(kBlocks + (i & 0xFFFF) * kBlockStride); }
}  // namespace pgr2

// thiscall (block, async), ret 8: starts reading a block's full version. One queued before the block was loaded here can still come.
X86_FUNC(sub_00100550) {
    if (pgr2::FullTextureHeld(kernel::R32(ctx.esp + 4))) {
        ctx.esp += 12;
        return;
    }
    __imp__sub_00100550(ctx, base);
}

// thiscall (block), ret 4: a block asked for this frame, which puts its full version back if it had gone to the small one
X86_FUNC(sub_00100CF0) {
    if (pgr2::FullTextureHeld(kernel::R32(ctx.esp + 4))) {
        ctx.esp += 8;
        return;
    }
    __imp__sub_00100CF0(ctx, base);
}

// thiscall: makes the city's cache
X86_FUNC(sub_00100990) {
    pgr2::ForgetAll();
    __imp__sub_00100990(ctx, base);
}

// thiscall (async), ret 4: each frame with 1; with 0 at a load, the file open for plain reads, it reads all that is asked for
X86_FUNC(sub_001078F0) {
    if (!kernel::R32(ctx.esp + 4)) {
        const uint32_t self = ctx.ecx;
        pgr2::LoadAll(ctx);
        ctx.ecx = self;
    }
    __imp__sub_001078F0(ctx, base);
}

// thiscall (count, records, ...), ret 0xC: both add blocks and return the first one's number; 0x1072D0 for textures with a stream part
X86_FUNC(sub_00107260) {
    __imp__sub_00107260(ctx, base);
    pgr2::MarkBlocks(ctx.eax, false);
}
X86_FUNC(sub_001072D0) {
    __imp__sub_001072D0(ctx, base);
    pgr2::MarkBlocks(ctx.eax, true);
}
