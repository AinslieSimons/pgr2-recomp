# PGR2 recomp: a review guide

A written walk through this project for review: how a static recompilation of Project Gotham Racing 2 (original Xbox)
works here, end to end, how the macOS port uses Metal, and a record of every change. Written from the code, the docs
(`CLAUDE.md`, `MACOS.md`, `FINDINGS.md`, `PROGRESS.md`, `sdk/README.md`, `sdk/harness/README.md`) and the git history of
both repositories as of 2026-11-10. Nothing was run to write it.

| Part | File | Covers |
| --- | --- | --- |
| 1 | [1-recompilation.md](1-recompilation.md) | XBE parsing, XDK signatures, the configs, the recompiler (analysis, C++ generation, flags, x87/SSE, `sse_approx.h`), overrides, the build |
| 2 | [2-cpu-runtime.md](2-cpu-runtime.md) | guest memory, kernel and XDK HLE (D3D8, DirectSound, XInput, XNet), threads and ARM ordering, pacing, input, audio, saves, the Windows and macOS hosts, the harness |
| 3 | [3-gpu.md](3-gpu.md) | D3D8 state to `gpu::State`, push buffers, the render thread, draw filters, vertex assembly, shader generation, W-buffer depth, targets and scaling, the reference renderer, D3D12/D3D11 |
| 4 | [4-metal.md](4-metal.md) | HLSL to MSL, pipelines and `metalcache`, encoders and parallel encoding, rings and frames in flight, MSAA twins, presentation and sizing, samplers, read-backs, visibility queries, ray traced reflections, baked AO, the performance recorder |
| 5 | [5-changes.md](5-changes.md) | every change by theme, in time order, with files and status; open issues at the end |

Conventions: files are cited as `path`, functions by name, guest addresses as `0x...` (PGR2's, the same in the PAL and
NTSC releases). "Inferred" marks anything concluded rather than read. Commit ids without a prefix are the main
repository's; "SDK" ones are the `sdk/` submodule's.

## One-page overview

**What it is.** PGR2's x86 code is translated ahead of time into C++ and compiled for the host (Windows x86-64 or
Apple silicon macOS). The Xbox kernel and the XDK libraries the game linked (Direct3D 8, DirectSound, XAPI, XNet) are
replaced at the API level by host code. No game files or generated code are ever committed; the player supplies their
disc.

```
 default.xbe ──xbe_symbols──► config/game.toml ─┐
 sdk/xdk/5659 (signatures, layout) ─────────────┼──xbe_recomp──► generated/game/*.cpp (one C++ function per guest function)
 config/pgr2.toml (hand entries) ───────────────┘                         │
                                                                          ▼
 src/game (X86_FUNC overrides) + sdk/runtime (kernel, XDK, GPU, host) ─► clang ─► PGR2 executable
```

**At run time.** Guest threads are host threads running recompiled functions against a 4 GB guest address space. When
the game calls a D3D8 function, `sdk/runtime/xdk/d3d8.cpp` updates a `gpu::State` and records the call for a render
thread, which hands it to a backend: Metal on macOS (`gpu/metal.mm`), Direct3D 12 or 11 on Windows. Shaders are
generated as HLSL from the game's vertex programs, register combiners and fixed function state (`gpu/shaders.cpp`);
on the Mac `gpu/msl.cpp` rewrites that HLSL into Metal Shading Language by text. Sound is a software mixer standing in
for the APU; input comes from SDL3; frames are paced by the game's own present interval (menus 60, races 30, or the
`race_fps` setting).

**Where PGR2 differs from the generic SDK.** Everything game-specific is in `src/game` as whole-function overrides
(`X86_FUNC(sub_XXXXXXXX)` replacing the recompiled weak function) plus `src/common/game_info.cpp` (settings, menu,
hooks). FINDINGS.md documents the game knowledge each override relies on.

**The macOS port** (Mark, 2026-11-05 on) added: an SDL3/Metal host; a Metal backend with a shader cache, parallel
encoding, MSAA, four frame rings, and a performance recorder; and new visuals beyond the console: planar water
reflections, cars in the reflection cube and a cube per car, ray traced car reflections, baked ambient occlusion,
higher car/mirror/tree detail, full-size textures, sun shadows on every car. It also fixed issues that ARM exposed (an
audio ring crash from memory ordering) and one that affected every platform (RSQRTSS/RCPSS must answer as the Pentium
III does, or replays and demos drift). Frame shape work made menus 4:3 with bars and filled 16:10 screens.

**State of play** (details in part 5, section 5.21): the Mac build plays menus, races, replays and the demos; most
macOS-era changes are confirmed on the Mac only and have not been built or checked on Windows. Open items include SMAA
on Metal, MSAA for water and cubes, a one-pixel line by the tachometer numerals with MSAA, missing small trackside
objects in one Hong Kong race, and the large AO files.

**Testing without a GPU.** `sdk/harness` runs the same code headless with a CPU reference renderer;
`scripts/smoke.sh` must print SMOKE OK before `main` moves. On the Mac, `scripts/replay-test.sh play demos` replays a
recorded controller through the five built-in demos with fixed screenshots, the project's repeatable visual test.

## Glossary

- **XBE**: the Xbox executable format. **XDK**: Microsoft's Xbox development kit; build 5659 for PGR2.
- **HLE**: high-level emulation; replacing library or kernel functions with host code at their API.
- **Guest / host**: the original Xbox program and its view of memory / the PC or Mac running it.
- **NV2A**: the Xbox GPU. **Push buffer**: the command stream the XDK writes for it. **Register combiners**: its
  pixel shading stages.
- **W-buffer**: depth stored as the pixel's w (distance), which PGR2 uses on nearly every draw.
- **Twin**: a multisampled copy of a render target that draws go to under MSAA, resolved into the real one.
- **Ring**: a per-frame Metal buffer for vertices, indices, constants and uploads.
- **Followed car**: the car the camera follows (usually the player's).
