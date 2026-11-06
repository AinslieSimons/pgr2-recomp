// The audio manager's command queue, made safe between CPU cores.
//
// The game thread sends commands (play a sound, set a volume) to the audio thread through a ring at [manager 0x458964
// +0x2BC]: +0 reset flag, +4 buffer, +8 size in dwords, +0xC read pointer, +0x10 write pointer. 0x129BE0 writes a command and
// then moves the write pointer, with no lock; 0x129C80, on the audio thread, reads up to it. That is safe on the single core
// console and under x86's store order, but ARM may show the new pointer before the command behind it. The reader then takes
// old dwords for a command, loses its place in the stream and runs on through old data, reading arguments as opcodes and
// small numbers as sound ids. Sound ids below 22 index before the table in 0x12E680, where a slot may be null: the fault at
// guest address 4 in the crash of 2026-11-06 (PROGRESS.md).
//
// So only the audio thread touches the ring here. 0x129BE0 keeps a command in host memory under a mutex, and 0x129C80 first
// copies what has arrived into the ring, with the game's own 0x129BE0, then reads it as before. Commands still take effect at
// the audio thread's next pass, in order.
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_00129BE0);
X86_EXTERN_FUNC(__imp__sub_00129C80);

namespace {
using kernel::G;
using kernel::R32;
using kernel::W32;

constexpr uint32_t kManager = 0x458964, kManagerRunning = 0x57C;

std::mutex g_mutex;
std::vector<uint32_t> g_pending;  // op, argument count, arguments, for each command
uint32_t g_pendingDwords = 0;     // as the ring would hold them

// The game's own write, on the calling thread's guest stack. False when the ring is full.
bool WriteToRing(X86Context& ctx, uint8_t* base, uint32_t queue, uint32_t op, uint32_t count, const uint32_t* args) {
    const uint32_t esp = ctx.esp;
    ctx.esp -= count * 4;
    const uint32_t guestArgs = ctx.esp;
    for (uint32_t i = 0; i < count; i++) W32(guestArgs + i * 4, args[i]);
    X86_PUSH32(guestArgs);
    X86_PUSH32(count);
    X86_PUSH32(op);
    X86_PUSH32(0u);  // return address, unused: the callee only pops it
    ctx.ecx = queue;
    __imp__sub_00129BE0(ctx, base);
    ctx.esp = esp;
    return (ctx.eax & 0xFF) != 0;
}
}  // namespace

// bool Send(op, count, args), this = queue. As in the original: nothing while the manager is stopped, false when there is no room.
X86_FUNC(sub_00129BE0) {
    const uint32_t queue = ctx.ecx, op = R32(ctx.esp + 4), count = R32(ctx.esp + 8), args = R32(ctx.esp + 12);
    ctx.eax &= 0xFFFFFF00u;
    if (G<uint8_t>(R32(kManager) + kManagerRunning)) {
        std::lock_guard lock(g_mutex);
        if (g_pendingDwords + count + 1 < R32(queue + 8)) {
            g_pending.push_back(op);
            g_pending.push_back(count);
            for (uint32_t i = 0; i < count; i++) g_pending.push_back(R32(args + i * 4));
            g_pendingDwords += count + 1;
            ctx.eax |= 1;
        }
    }
    ctx.esp += 16;
}

// void Process(), this = queue, on the audio thread only.
X86_FUNC(sub_00129C80) {
    const uint32_t queue = ctx.ecx;
    static std::vector<uint32_t> batch;
    {
        std::lock_guard lock(g_mutex);
        batch.swap(g_pending);
        g_pendingDwords = 0;
    }
    size_t i = 0;
    while (i < batch.size() && WriteToRing(ctx, base, queue, batch[i], batch[i + 1], batch.data() + i + 2)) i += 2 + batch[i + 1];
    if (i < batch.size()) {  // the ring is full: what is left goes first next time
        std::lock_guard lock(g_mutex);
        g_pending.insert(g_pending.begin(), batch.begin() + i, batch.end());
        for (size_t k = i; k < batch.size(); k += 2 + batch[k + 1]) g_pendingDwords += batch[k + 1] + 1;
    }
    batch.clear();
    ctx.ecx = queue;
    __imp__sub_00129C80(ctx, base);
}
