# Part 2: the CPU side at run time

What the recompiled code runs on: guest memory, the Xbox kernel and XDK libraries reimplemented on the host, threads,
timing, input, sound, saves, and the host application (Windows and macOS). The GPU path starts in part 3.

Sources read: `sdk/runtime/kernel/*`, `sdk/runtime/xdk/*`, `sdk/runtime/cpu/dispatch.cpp`, `sdk/runtime/app/*`,
`sdk/runtime/mac/main.mm`, `sdk/runtime/core/input.h`, `sdk/runtime/audio/*`, `src/game/*`, `MACOS.md`, `FINDINGS.md`,
`PROGRESS.md`.

## 2.0 The shape of a running game

```
 host process
 ├── main thread (macOS: SDL event loop, window, keys; Windows: Win32 message loop)
 ├── guest threads (one host thread each, 64 MB stack): recompiled game code
 │     └── the game's drawing thread ("guest 14" in PGR2) calls D3D8 HLE → records commands
 ├── render ("drawing") thread: gpu::ThreadedBackend replays commands into Metal / D3D12 / D3D11
 ├── audio output callback (SDL3 on macOS, XAudio2 on Windows) pulls the DirectSound mixer
 ├── input: SDL3 controllers; Windows also a DirectInput thread for wheels
 └── worker threads: Metal shader cache builds, parallel encoders, AO bake (macOS)
 guest memory: one 4 GB reservation; base pointer passed to every recompiled function
```

Start-up (`sdk/runtime/mac/main.mm` `main`, the Windows `app/main.cpp` is the same in outline):

1. Log opened in `logs/` beside the executable; SDL initialised (video, audio, gamepad).
2. Settings loaded (`app::Load`, `pgr2.toml` beside the executable), then `GameInfo::init` (PGR2's
   `src/common/game_info.cpp` `Init`: options, draw filter, baked AO hook).
3. `app::FindGame` locates the game: extracted files in `assets/`, or a disc image (`.iso`) mounted as `D:` when
   `assets/` holds only `default.xbe` (`app/game_files.cpp`).
4. Window created (macOS: 1280 wide at the screen's shape, so a 16:10 display gets 1280x800; PROGRESS 2026-11-09),
   Metal view and layer made, ImGui initialised, `input::Init`.
5. `kernel::Boot(xbe, ...)` maps the image, sets up the kernel, and starts the title's entry point on a new guest
   thread. The main thread then only pumps events.
6. On close: the Metal shader cache is saved, the log flushed, `kernel::Shutdown`.

## 2.1 Guest memory (`sdk/runtime/kernel/memory.{h,cpp}`)

The guest is a 32-bit process; the host reserves the full 4 GB once and passes its base as `base` to every recompiled
function (on Windows the reservation is tried at fixed bases such as `0x100000000` so crash dumps are comparable; on
macOS `mmap` at the same hint). Layout (`memory.h`):

| Guest range | What |
| --- | --- |
| `0x00010000`–`0x7FFE0000` | user virtual space: the XBE image, heaps, `NtAllocateVirtualMemory` |
| `0x80000000`–`+128 MB` | physical memory ("contiguous"): the console's 64 MB, plus 64 MB only the host uses (copy targets, the DrawVerticesUP ring, a scratch push buffer), as a development kit has, so the title keeps its whole 64 MB |
| `0x80000000`–`+1 MB` | kernel data (exported variables) at the bottom of physical memory |
| `0xB0000000` | system memory: pools, thread blocks |
| `0xD0000000` | thread stacks (each with a guard page) |
| `0xF0000000` | the same physical memory again: the write-combined view |
| `0xFD000000` | host thunks (synthetic addresses for host functions the guest calls through pointers) |

The two physical windows map the same host memory twice (a file mapping on Windows; on macOS a POSIX shared memory
object, `shm_open`, unlinked at once, because macOS has no `memfd_create`). Commits are tracked per 4 KB guest page;
on Apple silicon the host page is 16 KB, so commit and decommit work on whole host pages and decommitted guest pages
are cleared by hand (`madvise` does not promise zeros on macOS). The contiguous allocator is best-fit, high first,
keeping the first 16 MB for requests that need it (D3D report pages); the top 128 KB are kept unused as on the console
(PGR's loading screen reads 80 KB past a buffer). `XBE_MEM_LOG` logs allocations.

## 2.2 The kernel HLE (`sdk/runtime/kernel`)

The XBE imports `xboxkrnl.exe` by ordinal. `kernel/xboxkrnl.cpp` registers host implementations with
`KERNEL_EXPORT(ordinal, name, argc)`; recompiled code reaches them through `x86_kernel` (part 1). An unimplemented
ordinal traps with its name. Areas:

- **Objects and waits**: events, semaphores, mutants, timers, DPCs. Dispatcher headers live in guest memory, written as
  the console writes them; waits use one host dispatch lock and condition (`objects.cpp`). User APCs run at the next
  alertable wait (the `ReadFileEx` completion path, `NtUserIoApcDispatcher`).
- **Threads** (`threads.cpp`): `PsCreateSystemThreadEx` creates a host thread per guest thread with a 64 MB host stack
  (recompiled code nests one host frame per guest call), a guest stack from `0xD0000000`, and a guest KPCR/ETHREAD/TLS
  laid out as the XDK reads them (`fs:` base points at it). New threads start with x87 control word `0x27F` and MXCSR
  `0x1F80`. Threads are named `guest <id>` for profilers.
- **Files** (`files.cpp`, `disc.cpp`): `D:` is the game (folder or ISO), the hard disk partitions are host folders under
  `hdd/` beside the executable (`\Device\Harddisk0\Partition1..5`); writing a FATX header to a partition formats it.
  The DVD authentication page is answered so XAPI's start-up passes. `GameInfo::disc_rate` can pace reads at the
  console drive's speed.
- **Memory** (section 2.1), **strings** and `RtlSprintf`, **crypto** (`crypto.cpp`: SHA-1, RC4, HMAC for signatures),
  **exceptions** (`RtlDispatchException` walks the `fs:[0]` chain like NT; `RtlUnwind` runs handlers for destructors).
- **Settings**: `ExQueryNonVolatileSetting` returns a fixed console: widescreen when the window is 3:2 or wider (else
  4:3), 480p, the chosen language (`XC_LANGUAGE`), PAL 60 Hz on a PAL console when a title asks.
- **Boot and relaunch** (`boot.cpp`): image sections mapped, kernel imports resolved; a title loading another XBE is
  relaunched into the matching executable (`GameInfo::images`). PGR2 has one image.

## 2.3 XDK HLE (`sdk/runtime/xdk`)

The XDK functions found by signature (part 1) are replaced by host code. Functions that only manage memory (resource
creation, locks, XG helpers) stay recompiled.

### Direct3D 8 (`xdk/d3d8.cpp`, about 2,400 lines)

The bridge from the game to the GPU path. It keeps the D3D device structure in guest memory consistent (the recompiled
getters read it), and turns each call into changes of a host `gpu::State` plus calls on a `gpu::Backend` (part 3). Key
behaviours:

- **Push buffers.** On the console D3D writes NV2A commands into a push buffer the GPU reads. Here state is set at the
  API, but some code still pushes raw commands (PGR2: four functions push vertex shader constants, `0x1852D0` etc.).
  Those go to a scratch buffer and `Walk` decodes them (constant loads `0x1EA4`/`0x0B80`, visibility tests, immediate
  mode, vertex program loads). Every setter reads pending pushed commands first, so the order matches the console
  (this fixed the player's car body being drawn with the wrong matrix, FINDINGS.md).
- **Swap and pacing.** `SwapFrame` handles `D3DSWAP_COPY/BYPASSCOPY/FINISH` (PGR2 draws its own copy of a
  supersampled back buffer between two swaps; presenting at each showed a white quad), rotates frame buffer memory as
  the XDK does, calls `Backend::Present`, checks achievements, then `app::Pace(PresentVblanks())`.
- **Present interval.** The title's `D3D_PresentInterval` (vblanks per frame) drives pacing: PGR2 runs menus at
  interval 1 (60 fps) and races at 2 (30 fps), as on the console.
- **Visibility tests** (occlusion queries) and **fences** map to backend reports; a title polling
  `GetVisibilityTestResult` without presenting gets the render thread kicked rather than waited for.
- **Render targets**: `SetRenderTarget`, `SetRenderTargetFast` (PGR2 draws its reflection cube faces with it),
  `SetVertexShaderInput(Direct)`, `SetDepthClipPlanes`, `Reset`, point sprites, palettes, gamma ramps, transforms (the
  XDK's numbering converted to Direct3D 8's), lights, vertex program memory (136 slots).
- **`DrawVerticesUP`** copies vertices into a 4 MB host ring the render thread reads.
- **Recorded push buffers** (`BeginPushBuffer`/`RunPushBuffer`) are supported for other titles; PGR2 never uses them.

### DirectSound (`xdk/dsound.cpp`, `audio/reverb.cpp`)

A software mixer replaces the Xbox's APU. Buffers and streams are read from guest memory as they play; Xbox ADPCM is
decoded; mix bins (the APU's routing of each voice to speakers and effect sends) and headroom are applied; pitch is
4096 per octave from 48 kHz. Completion callbacks run in `DirectSoundDoWork` on the title's thread, as the XDK runs
them. `DirectSoundGetSampleTime` is a host count of 48 kHz samples (the APU counter never moved on the host; PGR2's
menu clock ran from it and froze).

What the DSP effects image did on the console is partly reproduced (the image itself is not run):

- **Voice filters** (`SetFilter`): the APU's DLS2 low pass with the title's own cutoff and resonance maths, as a
  state-variable filter with glides and fades (added 2026-11-07).
- **I3DL2 reverb** on mix bin 10, driven by `IDirectSound_SetI3DL2Listener` presets (`audio/reverb.cpp`: early
  reflection taps plus an 8-line feedback delay network; 2026-11-07).
- **Effect sends heard dry**: a voice with no audible speaker bin plays its effect sends into the speakers (PGR2's
  in-race radio goes only to effect send 14), at `GameInfo::send_level` (the music volume).
- **Engine distortion** (effects 0-7): not modelled; an attempt was taken out (PROGRESS 2026-11-07).
- **Level ramps**: voice levels glide across each mixer block, new voices fade in over 64 frames and stopped ones out
  over 96, which removed crackle heard on the Mac (2026-11-07).

### XAPI and input (`xdk/xapi.cpp`)

`XInputGetState` and friends read `input::` pads (section 2.6); `XInputGetCapabilities` reports a wheel subtype when a
bound wheel steers port 0. `XapiVerifyMediaInDrive` passes (no disc to authenticate). **Pad record and playback**:
`XBE_PAD_RECORD=file` records controller 1's changes keyed to the title's own reads of it (PGR2 reads port 0 once a
frame), and `XBE_PAD_PLAY=file` plays them back, so a run sees the same presses on the same frames however long
loading takes. Since 2026-11-10 recording and playback also keep the frame step's fixed time, so a 60 fps race plays
back as recorded (main `821d7f8`).

### Networking (`xdk/xnet.cpp`)

System link over host sockets: XNet's winsock thunks are replaced; secure addresses map to real IPv4;
`XNetGetConnectStatus` answers connected (the library's answered "lost" with a null network object, dropping the
host); `--instance N` instances bind `127.0.0.N` for local tests; `join_ip` reaches a host past the LAN. Xbox Live calls
fail as not initialised.

### Other

`xdk/fibers.cpp` (fibers on `ucontext`; PGR2 has none), `xdk/crt_math.cpp` (CRT math on the host), `xdk/bink.cpp`
(unused by PGR2, which plays XMV movies through its own recompiled player).

## 2.4 Threads on ARM: memory ordering

x86 guarantees total store order; ARM does not. The recompiler emits locked instructions as sequentially consistent
atomics, but plain guest loads and stores are ordinary memory accesses, so lock-free guest code that publishes data
then a flag can be seen out of order on Apple silicon (MACOS.md, "Known risks"). What was found (PROGRESS 2026-11-06):

- **Audio command ring crash** (`src/game/audio_queue.cpp`): the game thread wrote a command then moved the write
  pointer with no lock; on ARM the 50 Hz audio thread could see the pointer before the data and index a table with a
  bad id (crash at guest address 4). Fix: `0x129BE0` queues commands in host memory under a mutex and `0x129C80` copies
  them into the ring on the audio thread, so only one thread touches the ring. Shown with forced delays; the natural
  crash was never reproduced, so its absence is not proof.
- **An audit** with ThreadSanitizer on a harness build whose generated code carried guest addresses as line numbers
  listed every racy handoff. Two "any core" races (racy on x86 too) were fixed: the stream slot stop
  (`src/game/stream_slots.cpp`, the stop now runs on the stream thread `guest 18` under its lock) and the frame time
  queue (`src/game/frame_times.cpp`, one host mutex). Others were judged harmless or low risk and listed.
- Acquire/release on every guest access was measured at roughly 8-9% CPU and not adopted.

## 2.5 Timing and pacing

- **Clocks**: `KeQueryPerformanceCounter` runs at the console's ACPI rate (3,375,000 Hz) from the host steady clock;
  `rdtsc` reads a 733 MHz count; `KeTickCount` and system time come from the host.
- **Pacing** (`app::Pace`, `mac/main.mm` and `app/main.cpp`, the same code): each `Swap` waits until the next frame time
  for the rate `PaceRate(fps_cap, vblanks)`: 60 Hz over the title's present interval, capped by the `fps_cap` setting.
  It sleeps in 1 ms steps then yields for the last 2 ms; logs "frames: N of M over X ms" every 10 s when frames were
  slow; and stands still while `pause_unfocused` and the window is behind (the audio callback also stops pulling, so
  the game's sample clock stops).
- **PGR2's race rate** (`race_fps`, 30 default; 60/90/120/144): `src/game/hooks.cpp` clamps the present interval and
  makes game time follow the clock (`0x2919AD`), so physics still takes fixed 1/60 s steps and a late frame takes more
  steps. Manual shift presses are kept until a physics step consumes them above 60 (FINDINGS.md, "The gearbox").
- **The frame fence**: PGR2's end of frame (`0x178300`) waits for the fence it inserted after the previous present,
  keeping the game at most one frame ahead of the drawing thread. That wait is time the game has to spare, not lost
  (FINDINGS.md, 2026-11-08).

## 2.6 Input (`sdk/runtime/core/input.*`)

`input::Pad` is the console report (digital buttons, eight analog buttons, two sticks). Sources: SDL3 game
controllers (Xbox, PlayStation, Switch; hot plug and rumble), the keyboard as controller 1 (two keys per console input,
remappable, saved in `[keys]`), optional mouse look, and on Windows DirectInput wheels and rigs with force feedback
(`core/wheel.cpp`, a thread of its own; stubbed on macOS). `GameInfo::pad` lets PGR2 adjust a polled pad. On macOS the
main thread pumps SDL events, which Cocoa requires; Windows virtual key codes are mapped to SDL scancodes
(`mac/main.mm`). Testing: `XBE_KEYS="seconds:vk:held ..."` presses keys by the clock.

## 2.7 Audio output

`audio::StartOutput(render)` pulls interleaved stereo float frames at 48 kHz. macOS: an SDL3 audio stream whose
callback calls the mixer (`mac/main.mm`, blocks of 1024 frames); `XBE_MUTE=1` silences it and `XBE_AUDIO_WAV=file`
records what the mixer produced. Windows: XAudio2 (`audio/xaudio2.cpp`). The headless harness pulls every 10 ms and can
write a WAV.

## 2.8 Saves and settings

- Saves are the console's: PGR2 writes `E:\UDATA\4d53004b\<id>\Profile.Sav`, which lands in `hdd/` beside the
  executable. Profiles are signed with the title key; a real Xbox profile cannot match the host's (zero) key, so
  `src/game` hands the loader its stored signature and saving signs it for this install (FINDINGS.md).
- The cache partition (`Z:`) is formatted and filled on first start (about two minutes behind the intro movies).
- Host settings (`app::Settings`, `app/settings.cpp`) live in `pgr2.toml` beside the executable: display, render
  scale, MSAA/FXAA, vsync, fps cap, keys, wheel binds and the game's own options (`GameInfo::options`).
- The in-game **Recomp** menu (`src/game/settings_entry.cpp`) is built from PGR2's own option page builders (it
  replaces Screen Center under Options > General Options, and is reachable from the pause menu). Since 2026-11-09 the F1
  settings window shows the same sections and rows (`GameInfo::menu`, SDK `fe3c971`), so the two never disagree; rows
  that do nothing on the Mac (Monitor, Wheel, SMAA, Fullscreen vs Borderless) are hidden there.

## 2.9 The host app layers

| Concern | Windows (`sdk/runtime/app/main.cpp` + others) | macOS (`sdk/runtime/mac/main.mm`) |
| --- | --- | --- |
| Window, events | Win32 window and message loop, splash, relaunch handover | SDL3 window with a Metal view (`SDL_Metal_CreateView`); SDL event loop on the main thread |
| Renderer | D3D12 (default) or D3D11, fallback to D3D11 | Metal only; settings name it `metal`, a d3d12/d3d11 file is read as Metal |
| Audio | XAudio2 | SDL3 audio stream |
| Wheels | DirectInput, force feedback | none (stub) |
| Game code | `PGR2.dll` compiled on first start (prebuilt model) or linked in | linked into the executable |
| Display | monitors, display modes, low-latency swap chain | the window's size; "display modes" are render sizes; Low Latency hidden |
| Message boxes | Win32 | `CFUserNotification` (NSAlert crashed outside an app bundle on macOS 27; SDK `e90f5c3`) |
| Keys | F1 settings, F12 screenshot, F3/F4 captures | F1, F12, F9 performance recording, F10 marker |
| Compat | native | `runtime/mac/compat/` supplies the Win32 and D3D12 names shared code uses, so `d3d_common.cpp` compiles unchanged |

Shared app code (`sdk/runtime/app`): settings, the ImGui overlay (`overlay.cpp`, the F1 window, built for controller
navigation), `perf.{h,cpp}` (the performance recorder, part 4), `game_files.cpp`, `game_code.cpp` (Windows DLL build),
screenshots, achievements (`core/achievements.cpp`: RetroAchievements-syntax conditions on guest memory, checked each
frame). `app::RenderScale` decides the render size; on the Mac 100% means the screen's size in points and the height is
the visible picture's (PROGRESS 2026-11-09).

## 2.10 The headless harness (`sdk/harness`)

The same recompiled code and runtime on Linux or macOS with stand-ins: no window (`app_headless.cpp`), a pad script
(`pad_script.cpp`), a null audio device, and `null_backend.cpp`, which counts frames, lists draws and draws chosen
frames on the CPU with `reference_renderer.cpp` (part 3). It is how the project tests without a GPU: `scripts/smoke.sh`
(title, Instant Action, race; must print SMOKE OK), `linktest.sh` (two instances over system link), `savetest.sh`,
`soak.sh`. Switches such as `REF=N`, `CAP=N`, `REF_DRAWLOG`, `poke` and `memdump` files are listed in
`sdk/harness/README.md`.
