# Part 5: every change, by theme

A record of the project's changes from the git history of both repositories, cross-checked with `PROGRESS.md`.

- **Main repo** (`pgr2-recomp`): 270 commits on `reflections`, which contains `main` plus 9 newer commits (2026-11-08 to
  2026-11-10) not yet on `main`. Authors: Harry (241, 2026-10-17 to 2026-11-07) and Mark Simons (29, 2026-11-05 on,
  the macOS port).
- **SDK** (`sdk/`, the XboxXBE-Recomp toolkit; submodule pointed at the AinslieSimons fork since main `835280f`,
  2026-11-08): 464 commits on `macos-port`, which contains the fork's `main` plus 9 newer commits. Authors: TGP482 (87,
  the upstream toolkit, 2026-09-27 to 2026-10-15), Harry (357), hk21x (1), Mark Simons (20).

Status words: **Mac confirmed** / **Windows confirmed** = seen working by a person on that platform (per PROGRESS);
**headless** = checked only in the Linux/Mac harness; **built** = compiles, not run; **open** = a known issue.
Commit hashes are short ids: plain ones are the main repo, "SDK" ones the submodule. Entries before the macOS port are
summarised more briefly; the macOS work (sections 5.10 to 5.19) is described in full.

## 5.1 The upstream toolkit (TGP482, 2026-09-27 to 2026-10-15)

Before this project: the XboxXBE-Recomp SDK made for Splinter Cell: Double Agent and BLACK. It brought the recompiler
(function discovery, jump tables, C++ exceptions, x87/SSE/MMX, overridable functions, hooks), the kernel and XDK HLE
for XDK 5849, the Direct3D 12 renderer with HLSL generation, DirectSound on XAudio2, SDL3 controllers, the settings
overlay, and the game template. Its last commits included "points cover their console pixel" and the "game code as a
DLL" model that Harry later adopted as an option (SDK `102285b`, 2026-11-02). Credits: XenonRecomp for the overridable
function layout, xemu and Cxbx-Reloaded as hardware references (sdk/README.md).

## 5.2 Bring-up: from XBE to a race (Harry, 2026-10-17 to 10-18)

| Date | Change | Problem solved | Files | Status |
| --- | --- | --- | --- | --- |
| 10-17 | XDK survey; 5659 seed from 5849, then `layout.toml` re-derived from PGR2's code and signatures signed (191/250 functions, 13/13 data) | PGR2 links XDK 5659, which the SDK lacked | `sdk/xdk/5659/*`, `config/` | done |
| 10-17 | Config split: `config/game.toml` scanner output only, `config/pgr2.toml` hand entries | scans overwrote hand edits | `config/*.toml` | done |
| 10-17 | Push buffer check: no Create/RunPushBuffer; pushed vertex shader constants decoded by `Walk` | the runtime does not replay recorded push buffers | SDK `9d6e390` | done |
| 10-17 | Hard disk partitions as raw volumes, `IoQueryVolumeInformation`, `NtUserIoApcDispatcher`, empty `FindFirstFile` names | XAPI's cache formatter failed | SDK `04830e1` | headless |
| 10-17/18 | Host `Reset`, `SetDepthClipPlanes`, `PlayEx`/`Pause`, `XapiVerifyMediaInDrive`, DVD authentication page | trapping stubs hit during boot | SDK d3d8, dsound, xapi, kernel | done |
| 10-18 | Swap honours COPY/BYPASSCOPY/FINISH | a flickering white quad (PGR2 draws its own copy of the supersampled buffer) | SDK `4c454c3` | Windows confirmed |
| 10-18 | Immediate mode under an FVF keeps every slot | movies had no texture coordinates (slot 9) | SDK `564262e` | done |
| 10-18 | `DirectSoundGetSampleTime` as a host count of 48 kHz samples | menus froze at their first letter | SDK `9e10046` | done |
| 10-18 | Pace by the title's present interval (menus 60, races 30) | races ran at double speed | SDK `3889abd`, `f82d6e3` | Windows confirmed |
| 10-18 | Smoke test (`scripts/smoke.sh`) required before every `main` fast-forward | regressions | `scripts/` | in use |
| 10-18 | Saves survive reboot; `savetest.sh`; soak and system link scripts | | `scripts/` | headless |

## 5.3 Rendering correctness (Harry, 2026-10-18 to 10-25)

| Date | Change | Problem | Where | Status |
| --- | --- | --- | --- | --- |
| 10-18 | Pushed constants read before any direct set (state setters read pending pushes first) | player car body off screen / no pixels | SDK `9688e51`, `72975c8` | Windows confirmed |
| 10-18 | `SetRenderTargetFast` on the host; cube faces drawn on the GPU copied into their cube | reflection cube faces all drawn into face 0 | SDK `f6f4edd`, `8770388` | Windows confirmed |
| 10-19 | `SetVertexShaderInput(Direct)` on the host | fences, lamp posts, signs missing | SDK `367d15b` | Windows confirmed |
| 10-19 | Draw filter hook (`gpu::g_draw_filter`); car shadow pass first without depth test, then 4096 units nearer | car shadow flicker | SDK `eb2b53c`, main `56c3109`, `1ba59ac` | Windows confirmed (nudge later turned off) |
| 10-19 | Edinburgh night building flicker traced to the game's own combiner mode (exhaust flames leave mode 5) | one-frame fullbright buildings | frame log tools (SDK `36c9a05`...) | fixed with W-buffering |
| 10-19/20 | **W-buffering**: D3DZB_USEW depth from perspective-correct w, polygon offset in w units, fixed function and pretransformed w scaled to depth units, on by default | missing showroom panels, under-car flicker, street light glows through buildings | SDK `9f82dd1`, `6e68f78`, `b5111d8`, `20464ad` | Windows confirmed |
| 10-20 | A depth buffer bigger than the colour target serves it | showroom: the far wheel through the body | SDK `0f1b57e` | Windows confirmed |
| 10-20 | Car shadow nudge off (it put the sun shadow on the car under W-buffering) | sunlit patches | main `b516747` | Windows confirmed |
| 10-21 | Point sprites (trees) | trees drawn as single pixels | SDK `4fdab5a`, `2a8760f` | to confirm on Windows |
| 10-22 | Depth buffer read as colour gives the D24S8 bytes | replay depth of field blurred everything | SDK `48103f0`, `34f1fd8` | Windows confirmed |
| 10-22 | Target read as a format without alpha reads alpha 1 | | SDK `c9a749c` | done |
| 10-24 | Draws with vertices outside guest memory left out | Paris DLC crash (garbage strides) | SDK `35459f8` | Windows confirmed |
| 10-25 | W-buffered draws not clipped past the far plane | Nurburgring flashing sky | SDK `03c2e5b` | Windows confirmed |
| 10-25 | Sharp text and HUD (`Sharpen2D`) | blurry 2D at high render scale | SDK `94a9acf` | Windows confirmed |
| 10-28 to 11-01 | Many generic fixes found with other games (EoN, NightFire, Agent Under Fire, Forza, PGR1): XDK 3925/3948/4039/4831 data, recorded push buffers, vertex program memory, MultiSampleMode, scissors, stencil defaults, vertex blending | other titles | SDK | gated by a six-game check |

## 5.4 Performance on Windows (Harry, 2026-10-20 to 11-07)

- Render thread per-draw cost trimmed (state diffs, memo tables, skipped unchanged pipeline and root arguments; SDK
  `cc2f95e`, `6ad4094`, `04b3517`).
- Stutter: DirectInput wheel scanning moved off the game's threads; visibility reports answered without waiting; the
  fence after a report kicks the render thread (SDK `7c0bccd`, `f259e89`, `1d264f6`; 2026-10-22). Windows: slow frames
  0-3 per 10 s, from up to 272.
- Races at 60: the reflection cube draws every other face each frame (each face at 30 a second; main `1e709dd`); race
  frame rates 30/60/90/120/144 with game time from the clock (main `219061d`, `82c56ad`, 2026-11-01).
- Shader cache (`shadercache.bin`, SDK `92b245d`) and pipeline cache (`pipelinecache.bin`, SDK `4d6b70e`).
- Cube face shrink pool on D3D12 (SDK `77b18eb`, 2026-11-07): 84 to 109 fps when busy.

## 5.5 Display and aspect (Harry, then Mark)

| Date | Change | Where | Status |
| --- | --- | --- | --- |
| 10-20 | Ultrawide: frame at the screen's shape, 3D views widened via PGR2's aspect constant, 2D squeezed into a centred 16:9 | SDK `31257a7`, main `7eba3c5` | Windows confirmed (32:9) |
| 10-20 | MSAA 2x/4x/8x via multisampled twins (D3D12), render scale with MSAA | SDK `359099a` | Windows confirmed |
| 10-22 to 10-27 | Title screen 2D, rear-view mirror camera box and viewport in ultrawide | main `48103f0`, `155896d`, `64c4170` | Windows confirmed |
| 10-31 | MSAA and FXAA separate, sharpening | SDK `bfb7f6e` | Windows confirmed |
| 11-01 | SMAA, sharp scaling filter, smooth alpha edges, low latency (all off by default) | SDK `bbe0e50` | Windows |
| 11-06 | Ultrawide by default; aspect choice latched at boot; 16:9 vs fill applies live | main `0d02a98`, `e9d0beb`, `d83c7f3` | Windows confirmed |
| 11-06 | Intro movies pillarboxed | main `74164bd` | Windows |
| **11-09 (Mark)** | **Menus at 4:3 with bars** (no menu code reads the widescreen flag, so the console stretched them); title-screen 2D at 4:3 over the wide attract demo; **16:10 filled** (4:3 mode widened to 1.6, HUD 4:3 in the middle, mirror narrowed); tachometer lines gone (Sharp 2D reads no more than a texel's own alpha at its centre) | main `adaa798`; SDK `d72d4a2`, `1cd1fa7` | Mac confirmed (2560x1600); Windows to confirm, especially the live frame shape switch at each menu/race change |
| **11-09 (Mark)** | **16:10 full screen**: the Mac window opens at the screen's shape so the game latches a 16:10 TV shape at boot | SDK `6e23d6b`, main `288ad67` | Mac confirmed |
| **11-09 (Mark)** | Render size from the visible picture; 100% = the screen's size in points on a Mac | SDK `2d36ea5` (`app/settings.cpp`) | built, then in use |

## 5.6 PC features: settings, input, wheels, launcher, releases (Harry)

- F1 settings window (10-19), redone in classic Steam style with sections and controller-first navigation (11-01);
  keyboard play and remapping; monitor, FXAA, FPS counter, pause on focus loss (SDK `1274a46`, `09ff967`, `38f3033`).
- PGR2's own Options menu gains "PC Settings", later **Recomp**, built from the game's own row builders (main `78bf500`,
  `1f51a4d`, `450ad67`), reachable from the pause menu (`6523e18`, `054d586`).
- Achievements: 64 conditions in RetroAchievements syntax on guest memory, pop-ups, `achievements.toml` (10-20/21).
- Steering wheels and rigs through DirectInput with force feedback, H-pattern shifters, pedals on any axis, wheel subtype
  reported to the game (10-21 to 10-28). Windows only.
- System link fixed (`XNetGetConnectStatus`, 10-20); join by IP.
- DLC: Paris and Long Beach city packs and the title update's 16 cars on the disc build (10-21 to 10-24; material
  arrays past 100 kept aside, pak layout conversion, checks against ContentMeta). Windows confirmed, experimental.
- Profiles from a real Xbox load (10-24); media-patched discs accepted (10-23); the game's eight languages (10-30).
- Sound: the in-race radio heard (effect-send-only voices played dry, 10-23), at the music volume (10-24).
- Launcher (`PGR2Launcher.exe`): builds from the player's disc, "your music" as custom soundtracks, updates; then the
  **prebuilt model** (11-02/03): releases ship the runtime and compiler, the first start compiles the game code DLL.
- Releases 1.0 (10-22) to 1.6.1 (11-07).
- Enhanced World Textures added (11-01) and later removed (11-07, Mark: smoothing only, 130-316 ms stalls).

## 5.7 Bug fixes found on Windows in 1.6.x (Harry, 2026-11-06/07)

Issue 30 (speed camera photo black: read back into guest memory as soon as it is taken, main `8da2e96`, `7da4edb`),
issue 28 (alpha to coverage keeps greater-or-equal 0, Washington's dome, SDK `1a68cd2`), issue 32 (Recomp from the pause
menu keeps the pause frame, `054d586`), issue 26 (intro movies pillarboxed). Windows confirmed for 30.

## 5.8 Level of detail and textures (Mark, 2026-11-05/06)

| Date | Change | Problem / measurement | Files | Status |
| --- | --- | --- | --- | --- |
| 11-05 | LOD measured (`PGR2_LOD_PROBE`, `lod_probe.cpp`): camera kinds, car detail formula, texture tiers | phase 0 of the visuals work | `src/game/lod_probe.cpp`, FINDINGS "Level of detail" | headless |
| 11-05/06 | **Car and mirror detail** (`car_detail` 0/1/2, `mirror_detail`), on by default (highest, full mirror) | distant cars and the mirror at low detail | `src/game/detail.cpp`, main `5f92747` | Mac played; no cost seen on M4 Pro; Windows untried |
| 11-06 | **Every streamed texture at full size** (`full_textures`): blocks inflated at the city's sync load into guest memory at `0x40000000`, swap and eviction held off | 10-27% of binds showed the small (8x smaller) version | `src/game/full_textures.cpp`, main `e659ecb` | Mac: 0% small; Windows untested |
| 11-06 | **Tree detail** at any distance (`tree_detail`) | trees thinned with distance | `detail.cpp`, main `6016e8a` | Mac; slight cost; Windows untested |
| 11-07 | Metal applies sampler LOD bias (macOS 26+) | distant textures softer than D3D | SDK `05b2ccf`, main `2ee485e` | Mac: 2-6% more edge contrast in races |

## 5.9 Car shadows (Mark, 2026-11-05/06)

- **Sun shadows on every car** (`cpu_car_shadows`, on): the console draws one shadow map and ground mesh, for the
  followed car; `src/game/car_shadows.cpp` overrides `0x21920`/`0x1892E0` to draw the map and mesh again for each
  other car in view (followed car first, then nearest first). Part of the port commit (main `2affceb`). Mac confirmed.
- **Overlapping shadows darken once**: shadow draws get a per-frame stencil mark that later ones skip (`draw_filter.cpp`;
  `1c40310`). Measured with a forced double draw; real overlaps not yet seen.
- **Hood and bumper views**: the game clears the followed car's map there but still runs the pass; the other cars'
  shadows now draw there too. Mac confirmed in a race.

## 5.10 The macOS port itself (Mark, 2026-11-05)

SDK `c984837` "Add a native macOS (Apple silicon) host with a Metal renderer" (25 files, +3,758 lines) and main
`2affceb` "Port PGR2 to macOS".

- **Host**: `runtime/mac/main.mm` (SDL3 window and event loop on the main thread, display settings, Windows key codes
  to SDL scancodes, SDL3 audio output, wheel stubs), `runtime/mac/compat/*` (Win32/D3D12/winsock names so shared code
  such as `d3d_common.cpp` compiles unchanged).
- **Renderer**: `gpu/metal.mm` (2,355 lines at first, following `d3d11.cpp`) and `gpu/msl.cpp` (HLSL to MSL by text);
  point sprites as instanced quads; the shader/pipeline cache in `metalcache/`; `hitch:` lines.
- **Shared fixes for arm64**: guest memory on 16 KB host pages with explicit clearing; `shm_open` instead of
  `memfd_create`; x86 intrinsics only on x86-64; `-ffp-contract=off`; fibers on `ucontext`; the harness builds with BSD
  tools; a disc image used when `assets/` holds only `default.xbe`.
- **Build**: `add_xbe_image_mac` in `XbeRecomp.cmake`; `scripts/mac-app.sh` (Dock app); `MACOS.md`.
- **PGR2**: intro movies always skipped (`PGR2_SKIP_INTRO=0` for pad scripts); CPU car shadows (5.9).
- Not in the first version: MSAA, SMAA, LOD bias, wheels, F3 captures. Shared changes were not compiled on Windows at
  the time.
- **Status**: menus, movies and races draw; keyboard and controllers played by hand; audio checked by ear (MACOS.md).

## 5.11 Threads on ARM (Mark, 2026-11-06)

| Change | Problem | Files | Status |
| --- | --- | --- | --- |
| Audio command ring touched only by the audio thread | crash at guest address 4 (`0x11D590`): on ARM the ring's write pointer was seen before the command | `src/game/audio_queue.cpp`, main `f0e7d9a` (branch `crash-11d590`, merged `a05a219`) | reproduced only with forced delays; fix run 318 s clean; rare crash cannot be proven gone |
| Audit of lock-free handoffs (ThreadSanitizer harness, spin monitors) | x86-ordering assumptions | PROGRESS 2026-11-06 (no code) | report |
| Stream slot stop on the stream thread, frame time queue under a mutex | two races unsafe on any core | `src/game/stream_slots.cpp`, `frame_times.cpp`, main `021b82c` | headless + TSan clean at those sites; not run on Metal/Windows at the time |

## 5.12 Visibility reports and the race-end freeze (Mark, 2026-11-06 and 11-08)

- `Flush` waits for the newest committed buffer, not only its own: the game froze at a race's end polling ~4,000 light
  tests (SDK `7a53864`, main `d6ef4e5`). Mac confirmed (replay works).
- Reports without waiting (`Kick` commits and goes on; completion handlers deliver): slow frames ~27-39% to ~9-10%
  in the demos (2026-11-08, SDK `2d36ea5` range). Post-race screens garbled without the wait, so PGR2 asks for it there
  (`gpu::g_reports_wait`, `src/game/hooks.cpp`). Built; the post-race screen not re-seen at the time of writing.

## 5.13 Sound (Mark, 2026-11-07)

| Phase | Change | Files | Status |
| --- | --- | --- | --- |
| 0 | What PGR2 asks of the effects: reverb on bin 10, DLS2 filters, engine distortion; no Doppler | FINDINGS "Sound" | done |
| 1 | Voice low-pass filters with glides and fades; **crackle fixed** by ramping levels across mixer blocks and fading voices in/out | SDK `8de67db`, `dsound.cpp`; main `185bfcd` | Mac: heard, crackle gone; every platform |
| 2 | **I3DL2 reverb** on the host (`audio/reverb.cpp`) | SDK `1b26752`, main `9c0eb8c` | Mac: "sounds good"; SMOKE OK |
| 3 | Engine distortion | tried and removed (5-10x louder, clipping; DSP code scrambled) | main `230e90e` notes | stopped |

Also `XBE_AUDIO_WAV` recording and `XBE_MUTE` on macOS.

## 5.14 Water reflections (Mark, 2026-11-06 to 11-09)

| Date | Change | Files | Status |
| --- | --- | --- | --- |
| 11-06/07 | **Planar water reflections** (`planar_water`, on): the main camera copied, mirrored in the water's plane, drawn into a 640x480 target before the main view; water draws (texture modes 1, 0x11, 0x11, 0xC, level vertices) read it at their screen position, bent by the normal map (new shader mode 0xC with a 2D stage 3 in all renderers); oblique clip by the projection's z; runs only while water reached the renderer in the last 30 frames; own visibility test slots (a shared copy crashed after ~2 minutes) | `src/game/planar_water.cpp`; SDK `ad7c0d5` (`DrawAdjust::stage3`, shader mode, 16 MB header ring); main `2d963f3` | Mac confirmed (Sydney); reference renderer; SMOKE OK; Windows D3D12/D3D11 not compiled then |
| 11-07 | Header ring 1 MB to 16 MB | `render_thread.cpp` | fixed wrong textures and 4 fps with the pass on |
| 11-09 | **Area culling** (`water_area_cull`): the mirrored camera's sphere and box tests (`0x184AD0`, `0x184F10`) reject what lies outside last frame's water on screen; **size** (`water_scale` Off/50/75/Full via `gpu::g_small_target`) | `planar_water.cpp`, main `abd417a` | built; picture not checked |
| 11-09 | **Far detail** (`water_far_detail`, default 2): far small city blocks left out of the reflection | same | estimated 34% of boxes in dips; not measured with it on |
| 11-09 | **Water updates** (`water_update`, default 1): half rate while frames run over budget, with a trigger that ends (three slow of four, 2 s, backing off up to 16 s) | same | replayed over a recording: fires only on real dips |
| open | Small trackside objects missing in a Hong Kong race (cause unknown; suspect the object queue cap at 0x436D90) | | open |
| open | A test camera facing the city: 30 to 17.5 fps with the pass, cause unknown | | open |

## 5.15 Car reflections (Mark, 2026-11-07 to 11-10)

The plan's five steps (PROGRESS 2026-11-07): (1) every car reflects the live cube, (2) cars in the cube, (3) a sharper,
longer-reaching cube, (4) a cube per car, (5) self-reflections by ray tracing.

| Date | Step | What | Files | Status |
| --- | --- | --- | --- | --- |
| 11-07 | 1 | **Every car reflects the live cube** (`car_reflections`): car draws sampling the city's fixed DXT1 cube take the followed car's live R5G6B5 cube (`DrawAdjust::stage3`) | `src/game/car_reflections.cpp`, main `0eb0df8` | Mac: "looks good" |
| 11-07 | 2 | **Cars in the cube** (`cars_in_reflections`): at the cube camera's flush (`0x186B20`) the car manager queues its cars (`0x32010`), the followed car refused by `0x1C900`; plus the `ctx.esp += 4` early-return fix in two hooks | same, main `71b00fa` | headless 67 car draws a frame; no crash |
| 11-08 | 3 | Cube distance and object range settings, up to the main view's | same + Reflections page | Mac |
| 11-08 | 4 | **A cube per car** (`car_cubes`): eight more cubes made with the game's own (`0x1788F0`), the cube pass run again per other car with camera and place moved; each car's draws matched by their world translation; `car_cube_every` takes turns (default 3, then **1 since 11-09**, `1c9ae7e`) | main `7b058c8`, `1c9ae7e` | Mac played; every frame 20.8 ms vs 17.8 ms at 1 in 3 before later optimisations |
| 11-08 | 5 | **Ray traced reflections** (`traced_reflections`, off, Metal): part 4 section 4.12 | SDK `4b01716`, main `71b00fa` | Mac confirmed by eye with debug views |
| 11-10 | 5 | Rays test other cars' boxes first: GPU 42 to 18 ms a frame (Hong Kong demo, MSAA 4x, 2560x1600; 12.5 untraced) | SDK `44c0b8a`, main `821d7f8` | Mac measured |

A new **Reflections** settings page (Recomp > Reflections and F1) holds water, live, cars in reflections, every car's
own, distance, range, every face every frame, and ray tracing with its range and ray length.

## 5.16 The Metal renderer's growth (Mark, 2026-11-06 to 11-10)

| Date | Change | Files | Evidence / status |
| --- | --- | --- | --- |
| 11-07 | No Direct3D renderer row or Low Latency on macOS; renderer named `metal` | SDK `03f0566`, main `60ce558` | built on Mac; Windows not compiled |
| 11-08 | Drawing thread savings: skip repeated texture/sampler binds, NEON index scans, direct index writes | SDK `metal.mm`, `vertex.cpp` | drawing thread 12.4 to 11.1 ms; screenshots identical |
| 11-08 | **Parallel encoding** of reflection passes on GCD workers, then (11-09) of every pass to a game target | SDK `2d36ea5` | 21.0 to 19.0 ms drawing thread; ≤2/255 differences in ~28 pixels, cause unknown |
| 11-08 | Wide reads capped at 5 taps a side (replay/post-race blur 27.9 to 4.2 ms) | SDK `d3d_common.cpp` | user preferred the look; one run had glitches not captured |
| 11-08 | A cheaper replay blur tried and removed (broke replays and post-race menus) | | removed |
| 11-09 | Cheaper constant uploads (compare against a cached copy, skip fixed-function ranges) | SDK `2d36ea5` | constants 2.7 to 2.0 ms |
| 11-09 | **MSAA on the scene** (twins for screen-sized targets, resolve at pass end) | SDK `2d36ea5` | Mac in use; water and cubes not covered |
| 11-09 | **Frame ring fix**: 4 rings, 3 frames queued (mirror corruption, flickering HUD under MSAA 4x) | SDK `6e23d6b`, main `288ad67` | Mac confirmed clean; pacing restored |
| 11-09 | Centroid interpolation of texture coordinates under MSAA | SDK `6e23d6b` | did not fix the tachometer line |
| 11-09 | Message boxes via `CFUserNotification` (NSAlert crashed on macOS 27 outside an app bundle); denied privacy folders named | SDK `e90f5c3`, main `8cba24a` | Mac |
| 11-09 | **Baked ambient occlusion** (part 4, 4.13) | SDK `1e6797b`, main `ee2fdab`, `src/game/baked_ao.cpp` | Mac: same fps; screenshots 5-10% darker at 60%; open: large files, early meshes re-baked |
| 11-08 | **Performance recorder** (F9/F10, `perf/`, `analyze.py`), stages 1-2, GPU per pass, encoders.csv | SDK `app/perf.*`, `tools/perf/analyze.py`; `src/game/perf_zones.h`, `draw_filter.cpp` pass labels | Mac in use |

## 5.17 Replays and demos in sync: RSQRTSS/RCPSS (Mark, 2026-11-09)

PGR2's replays and demos store inputs and re-simulate, so the simulation must match the console bit for bit. The
recompiler computed RSQRTSS exactly, where the Pentium III gives a 12-bit table answer that the game refines; all five
demos drifted until their cars hit barriers, at any `race_fps`. `sdk/runtime/cpu/sse_approx.h` reproduces Intel's
tables (captured under Rosetta 2, checked for all 2^32 inputs) and `codegen_fp.cpp` emits it (SDK `42dff51`, main
`ee2fdab`; branch `demo-desync`). Status: Mac confirmed, all five demos complete their laps at 30 and 60; not proven
bit-exact against a console; x87 transcendental functions remain a possible source of last-bit differences; Windows and
Linux builds untested (same code path). Generated code must be regenerated (the build does it).

## 5.18 Repeatable testing (Mark, 2026-11-07/08, 11-10)

- Controller 1 recorded and played back keyed to the game's reads (`XBE_PAD_RECORD`, `XBE_PAD_PLAY`,
  `XBE_SHOT_READS`), and `scripts/replay-test.sh` (snapshot, record, play from a fresh copy of saves, settings and
  shader cache). The standard test is `demos`: the five built-in demo replays with ten fixed screenshots (SDK `77fdec4`,
  main `326e871`). Two playbacks gave 8 of 10 identical screenshots, Chicago's differing only in random tyre smoke.
- Harness `REF_TARGETS=1` captures every target of a frame (SDK `e7b8781`).
- Recording and playback keep the frame step's fixed time (main `821d7f8`, 2026-11-10), so a 60 fps race plays back as
  recorded; `PGR2_TRACE_DRAWLOG` logs each car draw of chosen frames.

## 5.19 Settings menu unification (Mark, 2026-11-09)

`GameInfo::menu` (SDK `fe3c971`): when a title gives its own menu (PGR2's Recomp sections), the F1 window shows the same
sections, rows and choices in the same order, plus Controls, Wheel (Windows), Achievements and Quit. Main `d7a673b`:
rows that do nothing on the Mac hidden there (Monitor, Wheel page, SMAA, Fullscreen vs Borderless), mouse rows removed
everywhere, Car Shadows and Water Area Culling added to the in-game menu. Status: built and used on the Mac; Windows not
checked.

## 5.20 Repository housekeeping

- The SDK submodule URL moved to the AinslieSimons fork holding the macOS commits (main `835280f`, 2026-11-08), after
  merging Harry's `origin/main` (`df49bae`, SDK `ef29910`).
- Branches: main repo `reflections` (current) is 9 commits ahead of `main`; SDK `macos-port` is 9 ahead of its `main`.
  Other local branches: `crash-11d590`, `demo-desync`, `planar-water`, `macos-port` (main repo).
- Upstream: a draft PR description was kept in `.local/upstream-pr.md`, not sent (PROGRESS status).

## 5.21 What is open, in one list

- Windows: none of the macOS-era changes (5.8 to 5.19) has been confirmed on Windows; several shared changes were not
  even compiled there (planar water, MSAA text, settings rows). D3D12/D3D11 need the planar mode 0xC and wide-tap cap
  checked.
- Metal: SMAA; MSAA for water and cubes; LOD bias before macOS 26; one-pixel tachometer line with MSAA; lines at console
  width unchecked.
- Water: missing small trackside objects in Hong Kong; the 17.5 fps view; area cull and size not checked by picture.
- Baked AO: large files until compacted; early meshes re-baked on the second run.
- Replays: bit-exactness against a console unproven; x87 transcendental precision.
- ARM memory ordering: two races fixed; other lock-free structures rely on x86 order (acceptable risk per the audit).
- Sound: the engine distortion effect is not modelled.
- Not started on Mac: `.app` bundle and launcher; steering wheels; F3 captures.
