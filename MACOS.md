# macOS (Apple silicon) port

Status and plan for running PGR2 natively on arm64 macOS. Upstream targets Windows (Direct3D, XAudio2) and tests on a
headless Linux VM; this port adds a third platform.

## Status

| Layer | State |
| --- | --- |
| Recompiler, generated code, kernel, XDK | Native arm64 |
| Window and main loop (`runtime/mac/main.mm`, SDL3) | Works |
| Renderer (`runtime/gpu/metal.mm`, `runtime/gpu/msl.cpp`) | Menus, movies and races draw; see the gaps below |
| Keyboard | Played by hand |
| Controllers | Played by hand; rumble and hot plugging not checked |
| Audio (SDL3) | Works, checked by ear |
| F1 settings window | Draws; its mouse and keyboard handling is untried |
| Steering wheels | Stubbed: `core/wheel.cpp` needs DirectInput |
| `.app` bundle, launcher | Not started |

### Renderer gaps

MSAA and SMAA are ignored (logged once), a sampler's LOD bias is applied only on macOS 26 and later (`lodBias`), border
colours snap to the nearest of Metal's three, and depth bias is only approximate on `Depth32Float_Stencil8`. A question the game would ask on Windows
("different release") is answered yes and logged.

## Build and play

Put a supported disc image in `assets/` and copy its `default.xbe` beside it, then:

```bash
cmake -S . -B build/macapp -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

```bash
cmake --build build/macapp --target PGR2
```

```bash
build/macapp/PGR2
```

Settings, saves (`hdd/`), logs and screenshots go beside the executable. The first start formats the emulated cache and
takes about two minutes, behind the intro movies. Keys are as in the README; on a Mac without a numpad the D-pad keys need
remapping. Testing switches: `XBE_KEYS="seconds:vk:held ..."` presses keys by the clock, `XBE_SHOT_EVERY=N` saves every
Nth frame, `XBE_OPEN_SETTINGS=1` opens the settings window. `tools/mslcheck` compiles a folder of shaders dumped by the
harness's `AUDIT=N` through the translator.

## Dock app and shader cache

`scripts/mac-app.sh` makes `~/Applications/PGR2 Test.app`. Opening it rebuilds what changed and starts `build/macapp/PGR2`.

`gpu/metal.mm` records each shader and pipeline a run needs in `metalcache/records.bin` beside the executable and rebuilds
them on background threads at the next start. Pipelines also go in a Metal binary archive, `metalcache/pipelines.bin`.
Records last until a shader generator source changes (the hash in `add_xbe_image_mac`). `XBE_NO_SHADER_CACHE=1` turns the
cache off. Each stall over 50 ms logs a `hitch:` line with its shaders, pipelines, uploads and read-backs.

Records hold Metal source translated from the game's own shader programs, so shipping them means redistributing material
derived from the game. Archives are also specific to a GPU family and OS, so they can't simply ship to all Macs.

## Sun shadows on every car

`src/game/car_shadows.cpp`, the `cpu_car_shadows` setting (on by default, F1 > Game). The console draws one depth map and one
ground mesh (`0x21920`), for the followed car, and gives every car a texture blob (`0x32AB0`). This draws the map and the mesh
for each other car in view as well: the followed car's mesh first, the others nearest first, then the followed car's map again.
Overlapping shadows take one darkening: `src/game/draw_filter.cpp` gives each shadow draw a stencil mark that later ones skip.

## Water reflections

`src/game/planar_water.cpp`, the `planar_water` setting (on by default, F1 > Game): the scene is drawn a second time, mirrored, where water reaches the renderer. FINDINGS.md, "Water reflections", has the method and costs.

## What changed for macOS

- `kernel/memory.cpp`: `shm_open` replaces `memfd_create`. Commit and decommit work on whole 16 KB host pages while the
  guest keeps 4 KB pages; decommit clears memory explicitly because `madvise` does not promise zeros.
- `runtime/mac/compat/`: the Win32 and D3D12 names shared code uses, so `d3d_common.cpp` compiles unchanged.
- `gpu/msl.cpp`: translates the generated HLSL to Metal's language by text. Constants become fields of a struct and
  helpers its member functions, so they read as HLSL's globals do.
- `gpu/metal.mm`: `gpu::Backend`, following `d3d11.cpp`. Point sprites are instanced quads, as Metal has no geometry shaders.
- `core/input.cpp`: the main thread pumps SDL events, which Cocoa requires.
- `app/game_files.cpp`: a disc image is used when `assets/` has only a `default.xbe` beside it and no game folders.
- `harness/build.sh`: no x86 flags off x86-64, `-ffp-contract=off`, BSD shell tools.

## Known risks

- **Memory ordering.** x86 guarantees total store order and ARM does not. Locked instructions are emitted as sequentially
  consistent atomics, but plain guest loads and stores are not ordered. Any lock-free guest code that relies on x86 ordering
  could fail rarely. If that shows up, the options are acquire/release accesses in `X86_MEM*` (slower) or running guest
  threads one at a time, as on the single-core console.
- **Floating point.** `-ffp-contract=off` stops clang fusing multiply-adds, which would change results. Results have not yet
  been compared with an x86 build, and NaN sign bits differ between the two architectures.
- **Fibers** use `ucontext` and are untested: `config/game.toml` lists no fiber functions for PGR2.

## Next

1. Play it by hand: keyboard, a controller, sound, the F1 window.
2. Compare frames with the Windows build, especially car reflections, which looked dark in the first captures.
3. MSAA and SMAA.
4. A `.app` bundle built from the player's own disc, with files in Application Support.
5. Soak tests for memory ordering: long races, saves, and system link against a Windows build.

## Redistribution

`sdk/` is MIT licensed. This repository has no licence file, so get the author's permission before publishing the port.
Never distribute game files or `generated/`.
