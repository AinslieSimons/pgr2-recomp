// Stopping a stream slot, made safe between CPU cores.
//
// The stream manager at 0x484468 keeps 12 slots of 0x1FC bytes from 0x484478. 0x176BB0, on the streaming thread (guest 18), runs
// them under the lock 0x485DB4. When a slot starts a new file, 0x176690 releases its stream and zeroes [slot+0x10], and only then
// clears the "has a stream" flag [slot+0x24]. The stop in 0x174CA0 (from the game and audio threads) checks that flag, loads
// [slot+0x10] and calls the stream with no lock, so it can call null or a released stream. That needs another thread between two
// instructions, which the single core console rarely allows; several cores (x86 too) make it a matter of timing.
//
// So the streaming thread makes the call: the stop leaves the stream to stop for 0x176BB0, which calls it first, still under the
// lock and only if the slot still holds that stream. The stop's flags are set at once, as in the original, so the slot reads as
// stopped and free straight away; the sound itself ends at the thread's next pass, within about 16 ms.
//
// 0x174A50, which stops and frees every slot at the end of a load, makes the same unlocked call before taking the lock for each
// slot. It only runs at those times, so it holds the lock (recursive) throughout.
#include <atomic>
#include <cstdint>

#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_00174A50);
X86_EXTERN_FUNC(__imp__sub_00174CA0);
X86_EXTERN_FUNC(__imp__sub_00176BB0);

namespace {
using kernel::G;
using kernel::R32;

constexpr uint32_t kSlotCount = 12, kSlotSize = 0x1FC, kSlotsOffset = 0x10, kSlots = 0x484468 + kSlotsOffset;
constexpr uint32_t kSlotStream = 0x10, kSlotPlaying = 0x21, kSlotHasStream = 0x24, kSlotStopped = 0x25;
constexpr uint32_t kHandleTable = 0x17EC, kHandleSize = 20, kHandleKindOffset = 4, kKindSlot = 2;
constexpr uint32_t kSlotLock = 0x485DB4, kOrdinalEnterLock = 277, kOrdinalLeaveLock = 294;

std::atomic<uint32_t> g_stop[kSlotCount];  // the stream to stop, 0 for none

void Lock(X86Context& ctx, uint8_t* base, uint32_t ordinal) {
    X86_PUSH32(kSlotLock);
    X86_PUSH32(0u);  // return address, unused: the callee only pops it
    x86_kernel(ctx, base, ordinal);
}
}  // namespace

// void Stop(handle), this = manager. A handle names a player or a slot; players keep the original path.
X86_FUNC(sub_00174CA0) {
    const uint32_t entry = ctx.ecx + kHandleTable + R32(ctx.esp + 4) * kHandleSize;
    const uint32_t index = R32(entry);
    if (G<uint8_t>(entry + kHandleKindOffset) != kKindSlot || index >= kSlotCount) {
        __imp__sub_00174CA0(ctx, base);
        return;
    }
    const uint32_t slot = ctx.ecx + kSlotsOffset + index * kSlotSize;
    if (G<uint8_t>(slot + kSlotHasStream) == 1) {
        g_stop[index].store(R32(slot + kSlotStream));
        G<uint8_t>(slot + kSlotStopped) = 0;
        G<uint8_t>(slot + kSlotPlaying) = 1;
    }
    ctx.esp += 8;
}

// void Service(), this = slot, on the streaming thread with the lock held.
X86_FUNC(sub_00176BB0) {
    const uint32_t slot = ctx.ecx, index = (slot - kSlots) / kSlotSize;
    const uint32_t stream = g_stop[index].exchange(0);
    if (stream && G<uint8_t>(slot + kSlotHasStream) == 1 && R32(slot + kSlotStream) == stream) {
        X86_PUSH32(stream);
        X86_PUSH32(0u);
        x86_icall(ctx, base, R32(R32(stream) + 0x18));
        ctx.ecx = slot;
    }
    __imp__sub_00176BB0(ctx, base);
}

// void StopAll(), this = manager.
X86_FUNC(sub_00174A50) {
    const uint32_t manager = ctx.ecx;
    Lock(ctx, base, kOrdinalEnterLock);
    ctx.ecx = manager;
    __imp__sub_00174A50(ctx, base);
    Lock(ctx, base, kOrdinalLeaveLock);
}
