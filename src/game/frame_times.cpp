// The queue of frame times, made safe between CPU cores.
//
// The callback 0xD9010 (registered at 0xD90CF, on the host's vertical blank thread) pushes a time onto a small queue, times from
// 0x381934 and the count at 0x381940, and 0xD8FC0 on the game thread pops the oldest, shifting the rest. Neither locks, and both
// read, change and write the count. ARM can also show the new count before its time. A pop could then take an old time or lose one.
// On the console the callback interrupts the game thread rather than running beside it. Both now run under one mutex.
#include <mutex>

#include "cpu/x86_ops.h"

X86_EXTERN_FUNC(__imp__sub_000D8FC0);
X86_EXTERN_FUNC(__imp__sub_000D9010);

namespace {
std::mutex g_mutex;
}

X86_FUNC(sub_000D8FC0) {
    std::lock_guard lock(g_mutex);
    __imp__sub_000D8FC0(ctx, base);
}

X86_FUNC(sub_000D9010) {
    std::lock_guard lock(g_mutex);
    __imp__sub_000D9010(ctx, base);
}
