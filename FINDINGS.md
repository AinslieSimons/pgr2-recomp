# PGR2 recomp - findings

What the work so far has established, by topic. PROGRESS.md has the dated log with the evidence for each item.
Each item is marked:
- **[console]**: how the Xbox, its XDK or the runtime behaves. It holds for any title and the fix is in the SDK.
- **[PGR2]**: the game's own code and data. Addresses are PGR2's (XDK 5659, title id 0x4D53004B).

## The image

- **[PGR2]** Releases: the North American `default.xbe` (region NA+Japan, version 5, SHA-1 ec6e902f...) differs from
  the PAL one (region rest of world, version 2, SHA-1 676895b8...) in 531 bytes, all in the signature (0x4-0x103)
  and the certificate (time stamp, region, ratings, version, signing keys). Every section is byte for byte the same,
  so every address in this project holds for both; the NTSC disc runs the smoke test unchanged. The signing keys
  differ, so a save signed by one release may not be accepted by the other.
- **[PGR2]** Media patched copies (pgr2-variant.iso, 2026-10-23: a rebuilt XISO of the PAL disc): every data file the
  same, all seven .xbe files (default, update, dashupdate, downloader, three demos) changed in one byte, the jge after
  XapiVerifyMediaInDrive (default.xbe 0x193964, file 0x183964) made a jmp, signature not redone: the patch that lets a
  modded console run the game from its hard disk. That code is XAPI start-up the host replaces, so the PAL and NTSC
  patched files (SHA-1 acd77633..., 702b79fd...; the NTSC one computed, the same byte) are listed as the same code.
- **[PGR2]** XDK 1.0.5659 for every library (XAPILIB, D3D8, D3DX8, XGRAPHC, DSOUND, XBOXKRNL, XMV, XONLINES, XVOICE,
  LIBCMT, LIBCPMT). XNet is linked inside XONLINES. The movies are XMV (42 files); there is no Bink.
- **[PGR2]** Push buffers: there is no CreatePushBuffer or RunPushBuffer, and the XPR bundles hold only textures.
  Four game functions push vertex shader constants by hand (0x1852D0, 0x185330, 0x1853B0, 0x185450). The runtime's
  `Walk` decodes those commands (0x1EA4 load slot, 0x0B80 constants), so this is handled.
- **[console]** 5659 needed its own `layout.toml`. The CDevice tail moved -0x10 from 5849, ZScale and SuperSample
  moved -4, and the front fields moved -4. Every offset is cited to a PGR2 instruction. Signatures:
  191 of 250 functions, 13 of 13 data.
- **[console]** A function the scan cannot match is left at 0 and becomes a trapping stub. A library function that
  runs its own code against device state the host never set up crashes or silently does the wrong thing. Every
  one of these was found when the game called it, then moved to the host and signed in:
  Reset, SetDepthClipPlanes, SetRenderTargetFast, SetVertexShaderInput(Direct), PresentInterval,
  XapiVerifyMediaInDrive, PlayEx/Pause, SetDopplerFactor, XNetGetConnectStatus, XNetUnregisterInAddr.

## Boot, files, saves

- **[console]** Cache partition. XAPI's formatter asks IOCTL_DISK_GET_PARTITION_INFO; whole partitions are now raw
  volumes. `FindFirstFile("*.*")` sends an empty name, which must match everything. NtUserIoApcDispatcher (the
  ReadFileEx completion) and IoQueryVolumeInformation (file sort keys) are now implemented.
- **[console]** The DVD authentication mode page (MODE SENSE 0x3E) must be answered, or XAPI init fails.
- **[console]** DirectSoundGetSampleTime reads an APU counter that never moves on the host. PGR2's frontend clock
  ran from it: menus froze at their first letter. It is now a host count of 48 kHz samples.
- **[PGR2]** Profiles are at `E:\UDATA\4d53004b\<id>\Profile.Sav` (30100 bytes, a 20-byte signature first). They
  are saved on creation, on leaving Options and on quitting a race. Byte 0x844 holds MPH/KM/H.
  `scripts/savetest.sh` checks this.
- **[PGR2]** The profile loader (0xA87B0) checks Profile.Sav's 20-byte signature as a roamable XCalculateSignature
  (Begin 0x18BBB4 with 0, End 0x18BC16) over the rest, keyed with the title signature key a real kernel takes from
  the certificate (XboxSignatureKey, zero on the host), then the version (5) and each section's length; a mismatch
  is 0x3EE, "damaged". A profile from a real Xbox (an Action Replay copy) can never match, so src/game hands the
  loader its stored signature and its own checks decide; saving signs it as this install's.

## Frame pacing and speed

- **[console]** Titles pace themselves with the present interval (0x80000000 + vblanks), and the runtime now paces
  every Swap by it. PGR2 runs its menus at 60 (interval 1) and its races at 30 (interval 2), as on the console.
  Pacing by `fps_cap` alone ran races at double speed.
- **[console]** Swap honours COPY / BYPASSCOPY / FINISH. PGR2 draws its own copy of the supersampled back buffer
  between Swap(2) and Swap(4); presenting at every Swap showed that copy pass as a flickering white quad.
- **[PGR2]** The frame step is FUN_000D90F0: dt = interval / refresh, and physics takes fixed 1/60 steps
  (FUN_000F5D60). The interval setter is 0x177C70. `race_60fps` clamps 2 to 1, so a race at 60 runs the same
  simulation as at 30 (an experiment, off by default).
- **[console] Replays are inputs, simulated again.** PGR2's replays and demos hold the inputs (a minute of six cars in
  3 to 8 KB), so the simulation must match the console to the bit. RSQRTSS/RCPSS must give Intel's 12 bit table
  answers, not exact ones (the game refines them with a Newton step): exact answers sent every demo's car into a barrier
  (2026-11-09, `sdk/runtime/cpu/sse_approx.h`). race_fps does not change the simulation (physics steps 1/60 either way).

## Rendering: console behaviours the runtime now matches

- **[console] Pushed commands keep their order.** Raw commands a title pushes (constants, visibility tests) are
  read at the next draw. Every state and constant setter must first read what is pending, or a later direct set
  is overwritten by an earlier push. That bug put the player's car body off screen.
- **[console] W-buffering (D3DZB_USEW, ZENABLE 2).** PGR2 uses it on nearly every draw. Depth is the pixel's own w,
  interpolated perspective correct, plus the polygon offset in w units, stored as floor(w) in 24-bit units. xemu
  interpolates w linearly on the screen, which streaks long triangles. A Z buffer lost the showroom car's panels.
  - Vertex programs carry w already scaled to depth units, in their own matrices.
  - Fixed-function and pre-transformed (XYZRHW) vertices are scaled by the XDK by ZScale / far w:
    - SetTransform keeps 1 / far w of the projection.
    - 0x1C0FC0 folds the scale into the fixed-function matrix.
    - 0x1C0300 puts it in the pass-through program's c0.w.
  - SetTransform is on the host, so the runtime now computes this itself. Without it, lamp glows and their
    visibility probes showed through buildings.
- **[console] A depth buffer bigger than the colour target serves it.** The console uses the depth buffer's top-left
  part. PGR2 renders the showroom car into a 500x200 texture with the 640x480 depth buffer bound; dropping depth
  for the size mismatch let the far wheel show through the body.
- **[console] SetVertexShaderInput(Direct)** replaces the stream sources and the declaration for the draws that
  follow. Without it, PGR2's fences, lamp posts and signs read packed shorts as floats and vanished.
- **[console] SetRenderTargetFast** must switch targets on the host. PGR2 draws its six reflection cube faces with
  it. The faces drawn on the GPU are copied into the cube once a frame.
- **[console] Visibility tests.** Titles poll GetVisibilityTestResult inside a frame, so D3D_SetFence flushes when a
  report was asked for. In PGR2 a light glow's brightness comes from a visibility probe: an XYZRHW quad drawn
  between Begin and EndVisibilityTest (0x114FA0 / 0x114D20, 0x1C42F0).
  At a race's end 0x187DC0 waits for every pending test with no present in between (0x114CC0), so a report must not depend on a later frame to arrive.
- **[console] Other fixes:**
  - Immediate mode under an FVF keeps every slot written. The movies' texture coordinates are in slot 9.
  - A declaration longer than the stride reads the next vertex's bytes.
  - 8-bit targets keep their blue byte.
  - Only 32-bit linear surfaces are written back.

## Rendering: PGR2's own techniques

- **Trees** are point sprites: a vertex program sets each point's size (oPts, console pixels, 2 to 64) and the sprite
  supplies the texture coordinates; drawn as single pixels until 2026-10-21, every tree looked bare.

- **Car shadow.** The car's 1024x1024 D16 shadow map is projected onto a ground mesh built each frame
  (vs B0101931) with a polygon offset and no depth writes. With a Z buffer it failed the road's depth test by up to
  4096 units and flickered; `src/game/draw_filter.cpp` drew it 4096 units nearer. Since W-buffering that nudge is a
  quarter of a metre and put the shadow on the car's lower body in the sun, while the plain pass looks right: the
  nudge is off (`PGR2_SHADOW_NEARER=1` brings it back).
- **Car shadow draw.** A multiply (0, SRCCOLOR) with the depth map in stage 0. Its pixel shader outputs the vertex colour and never
  reads T0 in the combiners; the shape comes from the alpha kill on stage 0, which discards fragments the comparison leaves out. So the
  shadow is one flat shade per car, and only shadowed fragments reach blending and stencil. No draw uses stencil in a race (0 of
  7.8 million). With a shadow per car (`cpu_car_shadows`) `draw_filter.cpp` marks stencil on those draws alone, with a value that
  changes each frame, so a pixel takes one shadow. Draw filters run away from the guest call that made the draw (a flag set around
  `0x21920` also sees world and depth map draws), so match the draw itself, not the routine around it.
- **Polygon offsets** match the XDK exactly at 3x upscale. The layers that rely on them are road shadows, kerbs and
  decals.
- **Light pool under the car.** This is a 256x256 target drawn each frame, sampled in stage 3 by the road. The
  under-car flicker at night was much improved by W-buffering.
- **Reflections.** A 256x256 R5G6B5 cube (six faces via SetRenderTargetFast) is added as specular. Bump-mapped
  reflection shaders also sample a 256x256 reflection target.
- **Combiner modes.** PGR2 sets stage 0 through its own mode switch FUN_00105ED0 (cache 0x3F29A4): mode 1 is
  MODULATE(diffuse, texture) and mode 5 is SELECTARG1(texture). The exhaust backfire flames (FUN_00021620 via
  FUN_00021CD0, car +0x1508, 'EXHL'/'EXHR') leave mode 5 set. Draws with a pixel shader are not affected.
- **Car lighting.** The body's sun term is c56 = sun colour x the car's brightness (0x1F036). The brightness eases
  to 0.25 per wheel standing on an unflagged track triangle (bit 7 of the triangle's flag byte means shade). The
  data, the query and the test all agree with the console's code.
- **Depth of field (replays, the attract demo).** PGR2 copies the frame into a 640x480 target (0x03595000, read as
  LIN_X8R8G8B8), then draws it back over the frame (ONE, INVSRCALPHA) reading its depth buffer 0x03B20000 as a colour
  texture, LIN_A8B8G8R8 (format 0x3F): the console gives the D24S8 word's bytes there, depth's top byte in alpha, and
  the blur follows them. The renderers sampled every depth target as a shadow map (a comparison), so every pixel read
  as far and the whole picture blurred (Harry, 2026-10-22); a depth target read through a 32 bit colour format now
  gives its bytes (kTexDepthBytes).
- **Widescreen.** PGR2 reads XGetVideoFlags: widescreen renders anamorphic 16:9 into 640x480, with a wider field of
  view and the HUD moved out. It has no HD mode. More pixels come from the runtime's render scale.
- **Cameras and the rear view mirror.** Every camera is set up by 0x1857F0 (thiscall: its box x, y, w, h on the 640
  wide screen; box at +0x28..+0x34, applied by 0x185640), aspect at +0x38 = 0x2790F8 (16:9; 0x2790FC 4:3 without the
  widescreen flag 0x48614C) times the box's shape. The mirror of the bumper and hood views is a camera with the box
  220,38 200x70 drawn on the frame itself. Ultrawide (src/game/hooks.cpp): 0x2790F8 is the frame's shape, and a box
  touching neither side keeps its 16:9 aspect and its viewport (0x185640, from the box) is narrowed about the centre
  by 16:9 / that shape, as the HUD is squeezed; the box itself must stay, as the mirror's 2D frame is placed from it.
  Controller layouts: 0x34CC1C, 0xFC each, two button ids per action from +8 (action names 0x28A1F8, button names
  0x28F960); the default has CAMERA_NEXT on Black, CAMERA_PREV on White.
- **The reflection cube's cost.** The cube pass (loop at 0x180064, faces 0 to 5) is 30-39% of a race's draws and asks
  0x179290 (thiscall: 0, the face; always yes) per face. At 60 src/game/hooks.cpp answers yes to every other face, each
  face changing at the console's 30 a second. The title's frame needs about 1.7 ms headless; the render thread is the
  limit, about 3 us a draw. PGR2 points one texture header at each material in turn, so a key holding the render
  thread's copy of a header misses each time.
- **Option pages.** A page is a builder function returning 1 (General Options 0xA40A0, Game Options 0xA1710); the
  frame 0x2FEE08 holds the title's string id at +0x34. Rows: 0x58EE0 (cdecl, 17 arguments: x, y floats; 1; label;
  help line; an int; choice count; 2; 0; eight choice texts, all UTF-16 pointers) steps the int with Left and Right
  (0x4D130 on actions 2 and 3) and shows its choice; 0x59020 the same for a byte. The manager [0x2F4B78]: 0x4D3A0
  chains rows, 0x4D4C0 sends an action (4 A, 5 B) to a page builder, 0x4D270 focuses a row. The legend 0x28A488
  (0x89200 A's text, 0x891E0). src/game/settings_entry.cpp builds Recomp from these: one builder for the landing page and its sections, told apart by the row in focus when A sends it there, the manager's +0x2C.
- **Languages.** The game takes XC_LANGUAGE: English, Japanese, German, French, Spanish, Italian, Korean, TChinese
  from Frontend\<language>.ini and Cache\Resource; 9 Portuguese has files but runs as English. A missing
  Z:\<language>.Ind makes it format the cache partition (Z only) and copy again.
- **Wheels.** The controller poll 0x1375D0 (per port 0x124 bytes) opens a port on insertion and reads its
  XInputGetCapabilities (0x26D8E2) into +0xB96; the only reader of the subtype is 0x13787A, `CMP byte [ESI+0x5E],0x10`
  (XINPUT_DEVSUBTYPE_GC_WHEEL). Anything else gets a gamepad's treatment on both sticks: an adaptive dead zone (from
  0x27D894, moved between 0x27D898 and 0x27D894 as the stick is used) with the rest rescaled, then 0x137460, a learned
  stick gate (32 sectors from 0.75, the radius divided by the gate's), which bends a wheel's travel. A wheel gets its
  axes as they come. There is no wheel choice or wheel dead zone in the game's options. The SDK reports a port
  steered by a bound wheel as one (re-plugging it when that changes).

## Level of detail

Measured with `PGR2_LOD_PROBE=1` in a Florence street race (PROGRESS.md, 2026-11-05) unless marked as read from the code or data.

- **[PGR2] Street light car shadows** (2026-11-16). At night only (0x17FE7E) the race view runs 0x32D10: for each car in view
  the road plane under it (three points of the car, a cross product), the lights within 50 m from the grid 0x2D9808 (records
  0x54 bytes in [0x2D97FC], count [0x2D9800]; type +0x50 & 0x1F 2 or 3, place +0x38, radius +0x28, inner +0x2C, colour +0x24;
  strength 0x409E0, 0 past the radius), and per light the car's shadow hull (0xC7C00, triangle fans) flattened by the XDK's
  D3DXMatrixShadow (0x194C8C; d = P.L, M = d I - P L^T, the opposite sign to the D3DX docs) toward the light as a direction,
  divided by d (0x30AE0), drawn as a multiply (ZERO, SRC_COLOR) of base / (base + strength^2 colour). The draw is W-buffered:
  its w is its depth, so a shadow matrix scaled differently lands behind the road or the camera. The sun shadow's direction is
  0x49EE8C (the way the light travels, y < 0; -y scales its colour 0x49EEA8); its map 0x1892E0 and ground mesh 0x21920 (the
  road's collision triangles within 4 m, 0xBF3F0 on 0x381910) work at night too. `src/game/night_shadows.cpp`.
- **[PGR2] The light pool at night** (2026-11-16). 0x180230 (camera kind 3) draws, into target `0x48639C` (LIN_A8R8G8B8
  256x256, depth LIN_D16 from 0x178780: no stencil): the world, the cars and the light pass 0x42CE0 with colour writes RGB,
  then the followed car's headlight beam with alpha only (0x32850 -> 0x1F2A0 -> 0x1D4A0: one triangle strip in car space,
  y -0.3, forward -z, from z -(ac/2 - 0.2), half width +0xA8/2, to 12.7 m further, half width 10; blend ONE ONE). The
  road reads it in stage 3 (B1001F40: `t3 = lerp(c0, t3, v1.a)`, then `t3 * t0.a` added to the lit base). Measured,
  unbinding it takes all the light around the car; the strip is the beam ahead. A test that skips 0x180230 misleads: the
  road then samples a stale copy of the texture and stays lit. Car fields: +0xA4 radius, +0xA8 width, +0xAC length;
  `car+0xB68` lamps hold their place on screen for the camera being drawn, not in the car. `0x3CD804` keeps pointing at
  the pool's camera after the pass. Cutting cars' shadows out of the strip (headlight shadows) was tried and removed (2026-11-17; commits a652f4f, 22e7b39 on branch headlight-shadows).
- **[PC] Drawing from a hook** (2026-11-16). Begin/End vertices are inline commands the runtime reads at the next HLE call:
  Swap reads them after moving to the next back buffer, which is then cleared, so end with an HLE call. On Metal a draw
  reading a colour target in stage 0 into another target becomes a full screen depth copy (MetalBackend::DepthCopy) and
  the draw itself is dropped: read such a texture in stage 1 or above (found drawing the light pool over the frame, a debug view since removed). A pretransformed vertex at z 0 sits on the near
  clip plane.
- **[PGR2] Camera kinds** (`+0x40`): 0 main, 1 mirror, 2 reflection cube, 3 the camera of target `0x48639C` (0x180230, built by 0x1848D0), which stands at y -10000 until it follows the car and looks like the light pool's; the game has no planar reflection pass (2026-11-06, read from the code). Measured main: far 3000, object range
  x1, width 640, scale 1.06. Mirror: far 500, range x0.3, width 200, scale 1.13, view mode 4 only. Cube: far 30, range x0.1, 256 wide.
  The draw distance option does not change the mirror's far plane (0x291A08) or any object range.
- **[PGR2] Car detail** `car+0x15D0` (0x3210C, 0x321E9): trunc(3.34 x (log10(10 x cars in view) - 1)) + trunc(2000 / (width x radius x
  scale x 11 / depth)), the followed car 0. One level is about 7.3 m in the main view and 2.4 m in the mirror, as the formula gives
  for a radius near 2. Each part has up to 10 index sets over the same vertices (read from the disc's 86 cars).
  `src/game/detail.cpp` raises the float 0x2919C8 from 11 to 44 (high) or writes level 0 (highest); full mirror detail makes the mirror's far and range the main view's. Highest and full are the defaults (2026-11-06).
- **[PGR2] City geometry and placed objects** have one level (read from the data). Objects have a range at mesh +0x12: 50 to 1000, or
  65535 for always. Largest buckets seen: 575 of 1000, 163 of 2000, 29 of 400.
- **[PGR2] Textures** have two tiers: about two thirds of a city's have a small version always held (usually 32) and a full one in
  `.pak_stream` (often 256). Bit 0 of 0x455CB6 turns on the per-frame requests (0x11A49A); it reads 0x0B in a race, though no direct
  write sets it. A request needs distance / 100 <= the texture's class, 0 for nearly all, so about 100 m. The city's `TexCacheSize`
  did not limit Florence (8 MB): after the start it held 0.4 to 3.6 MB, with 8 MB only in the load before the race. Evictions kept
  up with loads. 10 to 22% of main view binds of streamed textures showed the small version, each 3 size steps (8x) short.
  Cache `+0x54` is not the bytes held.
  With `PGR2_TEX_DIST` at 250 m or more the small version share falls to 3 to 5% moving and under 1% parked, and the 8 MB cache then fills when parked, so the budget limits from there.
  On Metal at one parked pose per city (Florence, Chicago) the wider requests made no visible difference, and Chicago's 4.5 MB cache filled, leaving 14% small.
  Full versions total 55 MB in Chicago, 42 in Barcelona, 35 in Sydney, 34 in Stockholm, 28 in Edinburgh, 25 in Hong Kong, 24 in Yokohama and Moscow, 23 in Florence, 15 in Washington, 2 in the Nurburgring (each city's TEX chunk). A record with no full version has leftover bytes for its page count and size (Chicago 522 of 1714), which 2026-11-06's first totals of up to 305 MB wrongly counted; its compressed size is past its full size. The cache is one contiguous block, `[0x30D6BC + city x 0x68]` bytes, allocated at 0x100990 from the 64 MB. Forced larger on the harness: 48 MB fails the allocation (largest free block about 35 MB, 2026-11-06); 16 and 24 MB end in fatal error "m_CompressionStream.msg" from 0x136EC9 while the attract city loads (not traced). Chicago with requests at any distance and its own 4.5 MB: 13.5% of main view binds small on average, against 18.7%.
  A block's +0x10 and +0x14 are only written by 0x1072D0; blocks from 0x107260 keep the last city's, so flag 0x80 alone does not mean a stream part. A full version not asked for in a frame goes to its small header at once (0x100C00 at age 1 swaps +0 and +4), even while still held, and leaves after 20 frames. The cache has 250 slots. `src/game/full_textures.cpp` holds them all outside the cache.
- **[PGR2] Trees** take detail from size on screen (0x49780, pixels), not from a level per tree: 0x49F30 draws size x 0.0025 (at most 1) of the mesh's leaf points, limited by the range +0x12 and 300 (0x49B40, the game's only point list call), with a fuller shader past 0.5, and nothing below the int 0x2DF470. 0x49E70 picks one of 6 mesh levels from the table 0x2DF324..0x2DF374, largest first. `src/game/detail.cpp` reports a larger size.
- **[PGR2] Hood and bumper views** (0x179E90 false, view mode 4): 0x1D0C0 clears the shadow map (0x188C20) for the car it sits on instead of drawing it; the mesh pass 0x21920 still runs.
- **[PGR2] Reflections** (2026-11-06, harness draw lists with each shader's texture modes, Yokohama, Sydney, Chicago; stage 3 is sampled when its mode is not 0, stale bindings otherwise fill the list): the followed car's paint and glass sample the live 256x256 R5G6B5 cube; other 512x512 car textures sample a 256x256 DXT1 cube, read as each city's `FixedCubeMap` (the disc has it in all but the garage), as do world objects with a stage 3 reflection, scaled by the texture's alpha. Modes (1, 17, 17, 12) are bump-mapped reflection on the same cube, about 1300 to 2500 vertices a frame. Water is the level use of modes (1, 17, 17, 12), shader B0001C51/B0001FA0 in the runs measured: y -0.177 in Sydney (city 9), 2.0 in city 7, -5.497 in city 4, -4.790 in the smoke test's race; the same modes also draw things that are not level, so the filter tests the vertices. Not reflection: stage 3's 256x256 L8 target is the light pool (by day; at night it is LIN_A8R8G8B8 with a LIN_D16 depth, see "The light pool at night"); a 128x128 L8 and a 128x128 R5G6B5 are unidentified.
- **[PGR2] Which cube each car reads** (2026-11-07, per draw: stage 3's texture and kRsPsTextureModes). The followed car's
  draws bind the live cube (D3DFMT 5, R5G6B5 swizzled; 128x128 as the game describes it); every other car binds the
  city's FixedCubeMap (DXT1, 256x256), as do world objects. Car draws are told by their texture modes: paint 0x8401 (other
  cars) and 0x8421, glass and lights 0x8021, other parts 0x0421; world objects with the fixed cube use 0x0001, 0x0021,
  0x8001 and water 0x64621. The cube pass draws no car (no 512x512 car texture in any face): no car reflects any car.
  `src/game/car_reflections.cpp` (`car_reflections`, on by default) gives other cars' draws the live cube.
- **[PGR2] The cube pass and the cars.** The cube pass (0x180040) draws each face's world (0x117C40) and flushes the
  frame's draw lists (0x186B20, thiscall, the camera); the main view also has the car manager 0x297390 queue its cars
  (0x32010) before its flush, and later their other parts (0x32D10, 0x32AB0, 0x314E0). Cars: count at +0x0C, from
  +0x1C00, 0x6990 apart; 0x1C900 (thiscall, the car, al) says whether one is drawn, and like 0x32010 reads the camera
  being drawn at 0x3CD804 (holder 0x3B59C0 + 0x17E44) and its viewer's followed car (+0x10, then +0x140; view mode +0x128
  0xC follows none). `cars_in_reflections` (on by default) queues the cars at the cube camera's flush with the followed car
  refused by 0x1C900: 5 to 17 car draws a face, 67 a frame in the race measured. An override that returns early must
  pop the return address as the guest's ret does (ctx.esp += 4 for a plain ret): without it 0x32010 read its stack 4
  bytes off and faulted at guest address 0xC.

- **[PGR2] The reflection cube's camera and object** (2026-11-08, read from the code). The camera holder 0x3B59C0 has
  the cube camera at +0xBF30 (class table 0x27B780: update 0x185C50, draw 0x180040) and the light pool's camera at +0x10
  (0x27B764: draw 0x180230). The update sets the far plane (+0x18) from the float 0x291A0C (30), the object range (+0x20)
  from 0x275BA4 (0.1), the viewer from the main camera, and the place (+0xBEF8) to the followed car's raised by 0x275F58.
  The draw renders each face of the cube object at +0xBEF4: 0x4862D0, made by 0x1788F0 (thiscall: depth surface, size
  128, format 5; ret 0xC; cube texture at +0xA8, faces at +0x78, depth at +0xAC) and freed by 0x1786C0. A car's place is
  its +0x44, the same as the translation of its draws' world rows (c110 to c112).

- **[PGR2] The frame's fence** (2026-11-08, tally by caller, Metal, demos). Almost all of the game thread's fence wait is one call: 0x178300 (the end of a frame, thiscall) blocks on the fence it inserted after the last present (+0x1C), then presents and inserts the next. It keeps the game at most one frame ahead of the drawing, about 5 ms a frame when the drawing thread is the slower: time the game has to spare, not time lost. Dynamic data is written for the next frame on that promise and the drawing thread reads guest memory when it draws, so it should not be skipped. The DrawVerticesUP ring (4 MB) waited 0 ms.

## Water reflections

- **[PC] Planar reflections** (`src/game/planar_water.cpp`, `planar_water`, on by default). The main camera is copied, mirrored in the water's plane and drawn whole into a 640x480 target before the main view, and level draws with the water's modes read that target at their screen position, bent by the normal (`planar()` in `gpu/shaders.cpp`: mode 0xC with a 2D stage 3, which no console draw has). It runs only while water reached the renderer in the last 30 frames, and only for a full screen main camera; the mirror and split screen keep the cube.
- **Far detail** (`water_far_detail`, default 2; `PGR2_WATER_FAR`, F1 and Performance page): the box test `0x184F10` returns outside for the water camera's far, small blocks: 1 beyond 800 m under 20 m radius, 2 beyond 400 m under 50 m, 3 beyond 400 m under 100 m. In a Hong Kong race the dips are long sightlines: kept boxes for this camera go from 288 to 888 a frame, 67% over 400 m, and water draws are about 215 + 3.67 per kept box. Level 2 covers 34% of the boxes, an estimated 1,100 draws (2026-11-09, recording, not yet measured with the limit on). Spheres (objects, trees, cars, about 100 a frame) are not the lever.
- A camera's view is built inside its draw (vtable +0x1C, 0x188550: 0x187E90 from the viewer, or from +0xBEF4 when +0x14 is set), so writing +0xF0 before the draw has no effect; the hook on 0x188550 sets it.
- What is under the plane is clipped by the projection's z only: z = 0.001 x height over the plane. W-buffered draws are unaffected. Draws that are not W-buffered in this pass have not been looked at.
- **A camera's visibility tests are its own.** +0x1F0 holds 0xFA0 pointers to slots from one pool (0x40AD48, 0x4000 slots), +0x4074 the tests. A wholesale copy of the main camera each frame shared its slots and lost the ones it took: within about two minutes 0x115280 read through a null entry (guest address 0). The reflection camera is constructed by 0xF6340, is copied around those tables, and hands its slots back with the main camera's at a race's end (0x187DC0, then 0x187D90 on it).
- **The render thread's header ring** held 32768 copies (1 MB). PGR2 points one header at each material in turn, and a city drawn twice wrapped it before the render thread read the copies: wrong textures on everything and about 4 fps, on Metal only, as the reference renderer has no render thread. It is 16 MB now. The symptom looks like memory corruption or a broken race, and moving the targets did not change it.
- **Cost** (Metal, M4 Pro, render scale 3, 30 fps cap): Sydney route 115 in normal play holds 30 with 1 slow frame in 300, the pass running. A test camera at -1000 12 -200 facing the city: 30 off, 17.5 on, unchanged by halving the object range (+0x20), so the time is not in small objects; where it goes is not known. Facing out to sea from the same place holds 30.

## Tyre smoke and other particles

Read from the code only (2026-11-10, static analysis of the recompiled functions, no runs). Addresses are PGR2's.
The disc has no particle or effect data (no file or string names smoke or particles; `CarDynamics/*.ini` is handling,
`camera_settings.ini` the cameras, `Frontend.ini` debug switches): every value below is in the code.

- **[PGR2] The particle effects** live at 0xF5D60-0xFB800, each a pool of particle records, emitters holding linked lists
  of them, an update per frame and a draw (immediate mode, `D3DDevice_Begin` 0x1C0D40). They use the C library `rand()`
  (0x1B2944, MSVC's 0x343FD / 0x269EC3, 0 to 32767) and the frame step's seconds at 0x28F4EC. The car code drives them:
  per frame 0x20690 (updates), per car draw 0x21CD0 (draws), 0x1CD30 (clears), the wheel update 0x23670 (from 0x2507D).
  - Bouncing debris: 0xF7410 (spawn, `rand` in 0xF7130), 0xF74A0 (update: gravity 9.81 at 0x27B7CC, bounce off a ground
    height), 0xF7620 (draw).
  - Sparks or chips: 0xF9B20 (spawn N into an emitter, at most 200, from a pool of 400 records of 0x24 at 0x3D3F78;
    `rand` in 0xF9760), 0xF95B0 (particle update: gravity 10 at 0x275E48, velocity times 1 - dt, colour times 1 - 2 dt
    then each byte less 1 a frame; alive while it has colour; dt clamped to 0.5), 0xF9BB0 (update), 0xF9C30 (draw).
  - Something weather dependent, likely tyre spray: 0xFB3B0 (from 0x23670), reads 0x49EF70 against 2 (inferred: the
    weather); spawn 0xFA980.
- **[PGR2] Tyre smoke and dust** (the wheel emitter, at +0x134 in each wheel object):
  - Set up by 0xF87C0 (thiscall: max particles, delay; from 0x232C0 with 50 and 30): the emitter starts stopped, the
    delay counts down one a frame (0xF8D70) before its first particle. Particles come from the pool at 0x3CFAA8 (0xF89B0);
    an emitter's list holds at most 50 (0xF8C00, 0x32).
  - Steered each frame by 0xF87F0 (thiscall: wheel place, previous place, base colour, intensity 0 to 1, type; ret 0x14),
    which keeps the place and the last one, half the car's velocity as the particles' drift (+0x18B4.. x 0.5), and runs
    the emitter while the intensity is at least 0.1 (the double 0x27B820). By type: **1** the colour given, alpha
    intensity x 196 (0x277348): tyre smoke (inferred from the colour, the cases and the call site); **2** the colour x
    0.25 then 0x432A0(0x40, 0x30, 0x20), alpha x 200; **3** grey 0.75, alpha x 64 (0x27674C); **4** brown (0.6, 0.5,
    0.3), alpha x 200 (0x275E20): dust; **0** stops it. The wheel update 0x23670 passes 3 (0x23970), 4 (0x23BE0), 1
    (0x23CD3) and 0 (0x23D14, 0x23D88); the intensity is worked out in 0x23670 from the wheel (stored at its [esp+0x2C]
    by 0x2380E; not traced further), halved (x 0.5, the double 0x276078) where the surface's +0x08 is below 0.
  - Emitted by 0xF8D70 each frame: every particle is updated (0xF8630); while the emitter runs, each one that dies is
    replaced at once by a new one at a random point between the wheel's last and current places (0xF8A50); while it is
    stopped the dead are removed. With no particle made that frame and fewer than its maximum, a new one half the time.
    So the trail's length is the particles' life times the population (at most 50 a wheel).
  - A new particle (0xF8A50): size 0.15 m (0.1 for type 2), placed within +-0.15 m across and along (not type 2), rising at
    0 to 4 m/s, drifting +-0.5 m/s across and along, plus the drift given.
  - Its life (0xF8630, dt the frame step; type 3 counts it twice): it moves by its velocity, which falls by (1 - 4 dt)
    each frame (types 3 and 4 also lose 4 dt of upward speed); it grows, by dt / (6 x size) for type 1 (slowing: about
    sqrt(0.0225 + t / 3) metres at t seconds) and by dt for the others; its alpha (+0x13) fades in two stages:
    - from its start down to 60: alpha x (1 - 2 dt) a frame (times 4 dt for type 3), the same at any frame rate: about
      0.6 s from 196 (full intensity tyre smoke);
    - below 60: one less a frame whenever alpha x dt x 60 > rand() / 32768, which at 30 or 60 frames a second is every
      frame: 60 frames. **This tail is frame rate bound**: 2 s at the console's 30, 1 s at 60 (`race_fps` 60 and above),
      0.5 s at 120. Full intensity tyre smoke lasts about 2.6 s on the console and 1.6 s at 60.
  - Drawn by 0xF8BA0 (handed to the camera's draw lists, 0x186AB0, with 6.0), cleared by 0xF8F40.
- **[PGR2] Other frame counted effects**: the sparks' colour bytes also fall by one a frame (0xF95B0), and the smoke
  emitter's start delay counts frames (30: 1 s at 30, 0.5 s at 60).
- **[PGR2] The smoke draw 0xF8F50** copies an emitter's particles into 16 byte entries of an array on its stack (from
  esp+0x4C, frame 0x35C): room for about 50, so an emitter must not hold more (more ran over its callers' saved
  registers: a crash in the view 0x17FCC0, esi garbage).
- An emitter (the wheel's +0x134): +0x10 type, +0x14 stopped, +0x18 particles, +0x1C most it asks for (0xF87C0's
  first argument), +0x24 place, +0x30 last place, +0x3C drift, +0x48 colour, +0x4C start delay, +0x50 its list (+0
  head, +4 tail, +0xC cursor, +0x10 count; a record's link at its +0x28, the offset at its class's +4). 0xF8D20 adds
  one particle at the wheel (thiscall: type; pool take, list add, 0xF8A50). `rand()` keeps its state at +0x14 of the
  thread data 0x1B5E78 returns. The draw's first pass sums each visible particle's squared projected size (0x15D70 on
  the camera at 0x3CD804: out, place, 0; size x camera +0x130 x width x out +0xC, at most the width) and fades the
  emitter's alpha by min(1, 2 sqrt(width x height / sum)).
- Done in `src/game/smoke.cpp` (setting `smoke_duration`, Recomp > Detail "Tyre Smoke"): the pool has 4600 more records
  (5000), 0xF8630 is replaced by the same update with the
  alpha's first stage duration times slower and the tail one step each 1/30 s x the duration, `rand()` called as before.
- Also in `src/game/smoke.cpp`: cars stir the smoke (`smoke_physics`). Each particle's damping (1 - 4 dt) is towards
  the air's velocity at it rather than still air; the air is procedural, from each race car's place (+0x44) and velocity
  (dynamics +0x94 then +0x18B4) taken before the frame step: a wake of samples every 1/30 s kept 1.5 s (slipstream at half
  the car's velocity, spreading and dying with age; a Lamb-Oseen trailing vortex pair off the flanks (G 1.2 m x speed, core 0.5 m, wake kept 2.5 s, fading over 1 s), turning down between
  them and out along the road), potential flow round the body as it is now (a sphere scaled to a 2.2 x 0.9 x 0.7 m
  half-size hull, only ahead of the rear axle: as a 1.8 m sphere everywhere the rear wheels sat inside it, where the
  formula carries the air forward at more than the car's speed, and each car's own smoke rode along with it instead of
  trailing), a ground clamp at the height each particle was made (0xF8A50), and divergence-free curl-of-sines
  turbulence where the wake is. `PGR2_SMOKE_WIND="x y z"` adds a steady wind to every particle (a check that the air
  reaches what is drawn: 0 6 0 lifts the smoke in columns); `PGR2_SMOKE_LOG=1` logs the air at the particles every 10 s,
  and smoke takes the car's sun share (`smoke_lighting`: 0xF87F0's colour at the emitter's +0x48 times 0.45 + 0.55 x the
  car's brightness +0x150C, the wheel's car at its +0xD4; the emitter is the wheel's +0x134). The game itself shades
  nothing of the smoke but by the sun's colour (0x23620 multiplies the base colour +0xDC by 0x49EEA8).
- Also (`smoke_density`, "Smoke Particles", 300 by default): each wheel asks for 50 x the density (0xF87C0), its list
  holds up to 200 (0xF8C00 replaced), it grows density times as fast (after 0xF8D70, extra 0xF8D20 calls half the
  frames each, moved to a random point between the last and current place, `rand()`'s state put back after), each
  particle starts 1/sqrt(density) the size and grows to match (the same area of smoke), and 0xF8F50 is called for each
  48 of a longer list, each part's alphas scaled by the whole list's fade over the part's so a screen full of smoke
  fades as it would drawn at once.
- How long smoke should last, for the tuning: game particle smoke is usually 2 to 5 s a particle, randomised (Epic's
  Niagara smoke example 3 to 5 s; Unity's exhaust example 2.5 s, its 5 s default called too long for car fumes); real
  burnout smoke in still air dilutes over seconds to a minute or two, wind clearing it sooner. The console's 2.6 s
  (Console duration) is inside the game range; 1.5x to 2x (about 4 to 5 s) is the lingering end.
- A bug of ours (fixed 2026-11-10): the replaced 0xF8630 read `rand()`'s result from ctx.eax after `CallGuest`, which
  puts the caller's registers back, so each particle's 'random' number was its own address's low bits, fixed; where it
  was above 30 dt / duration the tail never fell and the particle never died (about half at Console duration, 60 fps).
  `CallGuest`'s return value is the guest's eax.
- The game's emission comes in waves: a wheel makes a particle only to replace one that died (and, below its most, one
  half the frames), so particles born together die together and, once lives are long enough to fill the list, the wheel
  makes nothing between waves (gaps at the wheel, the smoke looking thin and quick to go). smoke.cpp keeps a steady
  rate per wheel besides (50 a console life of 2.59 s, x the density), its most the rate x the life x 1.3 (at most 400;
  pool 10000).
- Smoke rising over the cars (2026-11-10, measured in tools/smoke-lab, drift circle 18 m/s): 28% of the particles above
  roof height with smoke_physics on, none off. The curl-noise turbulence moves smoke up and down alike but the road
  clamp stops the downward half, so it ratchets up; the trailing vortices, not knowing the road, lifted it too. Now the
  turbulence's vertical part grows from 0 at the road over 1 m and each vortex has its image under the road (the
  standard ground-effect model), and the heavier defaults (damping 2.5) keep it low: none above the roof without lift.
- A particle's own car (`own_wake`, 0 by default): a drifting or sliding car's own wake, vortices and body flow sent
  its smoke shooting off in all directions; now only other cars' air stirs it (the smoke still takes the game's drift,
  half the car's velocity, as it is made). The owner is the emitting wheel's car (emitter - 0x134 + 0xD4), set while
  0xF8D70 runs and kept beside each record; on demo 2 all 2000 checked were race cars in the list at 0x297390 (+0x1C00,
  stride 0x6990). In the lab (drift circle 18 m/s) mean smoke speed fell from 1.7 to 1.1 m/s.
- The game's cars for the smoke lab: `XBE_TRACE_DUMP=frame` (Metal, with `traced_reflections` on) writes that frame's
  traced scene, every car draw's triangles in the world with its world transform (c[110] to c[112], car_reflections.cpp)
  and material colour (c[147]; glass flagged), to trace_scene_<frame>.txt beside the exe. tools/smoke-lab/car_from_scene.py
  groups the draws into cars as BuildScene does (demo 2 frame 4000: two cars, 42 and 47 draws, about 14000 triangles
  each, 4.86 and 4.04 m long), puts one in its body draw's frame (the transform scales by 10; the up axis is the
  column nearest world y, the length the longer of the others) and writes tools/smoke-lab/local/car.json (git-ignored).
  Both cars came out back to front on the model's + axis; the windows (glass in the upper half; the headlamp lenses are
  glass too) sit behind the middle, which the script uses to turn it. The wheels' centres are about +-1.28 m along.
- What made the trail read as beads on the road rather than a plume (tools/smoke-lab, straight slide 15 m/s): every
  puff born opaque and the same size, nothing moving it once the cars had gone, one flat colour. Now (smoke.cpp, all
  in Tune): size and alpha varied per puff (+-45%, +-40%, host random numbers so rand()'s sequence stays the game's),
  faded in over 0.15 s, slow curl-noise eddies in all smoke growing with age (2.2 m/s at 0.25 /m, full at 1.5 s; the
  vertical part damped near the road), more warm rise (lift 1) and growth (1.6), and darker near the road (x0.7, full
  colour 2 m up). The record's +0x10 now holds what is drawn; the life's alpha and the colour as made are kept beside it.
- **[PGR2] No wind in the game**: no wind in the XBE's strings (only Windscreens.ini), the disc's text files or
  `Frontend\Cities.Ini` (per city: name, display name, draw distance, texture cache, audio, plates, radio, ID); the
  areas are paks and `<city> Frontend Maps.dat`; weather (0x383184) picks a lighting set. Cities.Ini's order is the
  race's city index at 0x38317C: 0 Edinburgh, 1 Moscow, 2 Barcelona, 3 Washington DC, 4 Chicago, 5 Florence,
  6 Stockholm, 7 Hong Kong, 8 Yokohama, 9 Sydney, 10 Nurburgring (11 Garage, 12 Showroom, then test areas); 9 and 7
  agree with the water heights found before. smoke.cpp's wind is ours: each city's approximate average (10 m up,
  climate records: 5.0 Edinburgh to 2.2 Florence) x 0.5 for street level, a random direction each race, gusts carried
  down the wind, weaker near the road. Lab: smoke over 2 s old moved 6 m downwind in Chicago against still air.
- Burnouts against skids (smoke.cpp, lab): a smoking wheel is a burnout by how slow its car is (full at a standstill,
  none from 8 m/s), and then a Lamb-Oseen vortex about its axle (the tyre's top turning forward, imaged under the road)
  swirls its smoke round the wheel and throws it back; lab: smoke within 0.8 m of the axle circles at 5.4 rad/s
  against 0.1 without. The car's frame for the axle is read from the car record before its place (rows at +0x14,
  +0x24, +0x34 of 16 bytes, or 12; the one that reads as a rotation; the model's +z points back), not yet checked in
  the game. A faster car's trailing vortices are tighter (core from 0.5 m at 15 m/s to half that at 30 m/s and up), and
  smoke follows strong car air faster (damping + 12/s, scaled by the air's speed over 5 m/s) so it can wrap round them.
- Burnout smoke like a photo of a real one (smoke.cpp, lab): that share of a burning-out wheel's puffs comes off the
  tyre itself, anywhere on the tread from the road up the back (140 degrees), across its 0.25 m width, leaving along the
  tread at 4 m/s (back along the road, then up the back of the tyre) and spilling out of its outer face at 0.6 to 1.8
  m/s, 1.6 times the size, 1.4 times the alpha, with no fade-in, and 1.5 times more of them at full burnout (the
  wheel's most allows for it). The car's speed and place come from its dynamics and +0x44 (physics on or off). In the
  lab's close-up beside the wheel the tyre is shrouded and the smoke rolls off its back and out behind; the draw's
  screen-coverage fade (0xF8F50) thins it when the camera is that close.
- Tyre smoke and the literature (2026-11-10): no study measures visible smoke from burnouts or drifts. Tyre wear
  particle studies (non-exhaust emissions; EEA guidebook, Mathissen et al. 2011 Atmospheric Environment 45, KIT drum
  tests) find the sliding part of the contact patch drives emissions: slip angle and force matter most (quadratic in
  lateral acceleration), road speed little, normal driving 1 to 9 mg/km (invisible). Visible smoke is rubber breaking
  down, products and particles changing around 300 to 500 C (most near 400 C). So smoke.cpp keeps a tread temperature
  for each smoking wheel: it heats with the wheel's intensity (the game's slip measure; 150 C/s at full) and cools
  towards the air (25 C) at 0.25/s plus 0.01/s for each m/s of the car; the smoke goes from 0.35 of full (a cold tyre)
  to full between 200 and 400 C, and a burnout's puffs off the tyre and its swirl with it. Lab: a burnout 158 C at 1 s,
  heavy from about 1.4 s, full at 4 s (405 C); a race start peaks near 100 C (light throughout); a drift at 18 m/s and
  0.8 settles near 300 C (0.69 of full). Smoke made in a building's shade is 0.8 of the sun's (was 0.45: with the
  darkening near the road it came out near shadow colour).
- `smoke_tune.txt` (beside the exe and pgr2.toml, or `PGR2_SMOKE_TUNE`), read again within a second of a change: every
  number of smoke.cpp's Tune ("name = value", # comments). Defaults are heavier than the game's (spawn 0.35 m not 0.15,
  rise 1 m/s not 4, spread 1.2 not 0.5, opacity x1.3, held 0.5 s, fade 1 not 2, growth 0.5 not 1/6, damping 2.5 not 4,
  lift 0.25); the game's are noted beside each. tools/smoke-lab is the same model in a browser with a stand-in car.
- Ways to make smoke linger (the first three now done): count the tail in time rather than frames (one less each 1/30 s), which gives the
  console's look at any frame rate; a slower fade (the 2 at 0x275C44 is shared with other code, so in 0xF8630 alone);
  a larger population (the 50 at 0x232ED and the list cap 0x32 in 0xF8C00), since longer lives with the same 50 make a
  thinner trail; the pool at 0x3CFAA8 holds all wheels' particles and its size is not yet read.

## The gearbox

- **[PGR2]** The player's gear is the signed byte at 0x29DACA + car x 0x6990 (the gearbox 0x29DAB0, +0x1A; -1 reverse,
  1 first; there is no neutral). The car record is 0x2973B0 + car x 0x6990 (speed at +0x19A0); the gearbox's
  +0x24 points to it. Shifts go through 0x19EB0 (up) and 0x19F60 (down) from 0x1A7B0: the automatic logic 0x1A580 when
  0x16520 says automatic, and the car's requests +0x1BC4 (up) / +0x1BC5 (down), which the pad's B and X set.
  Automatic: the byte 0x39B320 + player x 0x2498 (set at Select Transmission: Auto), or the car's mode +0x19C8 or the
  dword 0x39B31C + player x 0x2498 being 1 or 2. Measured headless with gdb watchpoints and shift logging, 2026-10-22.
- **[PGR2]** Shift requests are edges from the pad poll. The pad manager 0x457768 polls once a frame (0x1375D0, the only
  XInputGetState caller): device d's presses since the last poll are the mask at +0xBBC + d x 0x124 (held +0xBB4), and
  the poll zeroes the per pad gates 0x457758..64. The car's controls (0x16070 -> 0x18A40, controls at car +0x1BB8, so
  +0x1BC4/5 are its +0x0C/+0x0D) run in the physics step, and read actions 0x12 (up) and 0x13 (down) through 0x18740
  once per gate. Polls 120 and controls 60 a second at 120 (counted headless, 2026-11-03), so above 60 a press in a frame
  with no step is lost: 14 of 40 at 144 headless. src/game/hooks.cpp keeps the presses for the next step from 60 up.
  The harness changes its pad 60 times a second in step with physics, so at 120 it never shows the loss; 144 does.

## Time of day and weather

- **[PGR2]** Conditions are picked in FUN_000F14E0 (race setup) from per-city tables, after a rand() call. Night is
  0x383188 = 1 and weather is 0x383184 (2 = rain/overcast). Night in Instant Action is a normal random roll on the
  console too.
- Each city has three lighting sets (`Game\Areas\<city>.pak_day`, `pak_overcast`, `pak_night`), chosen at area load
  (0x116C20). The log line `pgr2: area loads with time of day T, weather W` shows the choice. `PGR2_NIGHT=1`
  forces night (an experiment).

## Xbox Live content

- **[PGR2]** Content packs are listed from T:\$C\<offer id> by XAPI's XFindFirstContent (0x192134) with a category
  mask: the disc build asks for 1 (car packs, 0x157180) and 2 (city packs, 0x1572F0: the pack's cities.ini and
  Routes.ini); the Paris and Long Beach booster packs are category 0x10 (ContentMeta.xbx +0x30), made for the title
  update (T:\$U, version 0x202, a different build). Each pack's header is checked against a keyed signature
  (0x19144B / 0x191366) and each file's against the metadata's table (0x156F00); community copies are unsigned
  until a console installs them. PGR2_DLC=1 lists them, passes the checks and puts the pack's object paks, audio
  files and ambience lines where the disc build reads them (src/game/dlc.cpp).
- **[PGR2]** A pack as downloaded lists each file's plain SHA-1 in its ContentMeta.xbx (all 20 of Paris's, all 18 of
  Long Beach's); a console's install re-signs them (0x155510, XCalculateSignature), so those cannot be checked off the
  console. With DLC on, the log says whether each pack's files match, and names any that do not. A damaged pak makes
  the city stop on PGR2's fatal error "m_CompressionStream.msg" (zlib 1.1.4 inflate failing in 0x136E70).
- **[PGR2]** Paris draws some of its garbage meshes with stream strides like 0xFFFFFFFC: their vertices lie gigabytes
  outside the guest's memory. The GPU renderers copied them and faulted in memcpy (the Windows crash a second after
  Paris loads); the SDK now leaves out any draw whose vertices lie outside committed guest memory (logged
  `gpu: a draw reads vertices outside the guest's memory`). The DLC cities and their routes are registered in the
  menus, after boot: route indexes past 145 are empty until then, and PGR2_ROUTE on one loads Edinburgh against it.
- **[PGR2]** City material object 0x3FE8C0, its slots in arrays of 100: pixel constants +0x0 (16 bytes), colours
  +0x640 (0x3FEF00), then material types +0x7D0 (0x3FF090, 8 bytes: shaders, vertex size in the low 6 bits of +7),
  kinds +0x8E30 (0x4076F0, a byte), animated textures +0x8E94 (0x407754, 0xC4 each, 64; each frame's stream block)
  and refs +0xBF94 (0x40A854, 128); counts +0xC194 on (0x40AA54 types, 0x40AA56 slots, 0x40AA5A animations,
  0x40AA5E refs). Slots are appended by the city pak loader 0x16CFA0 (through 0x16C0D0, then 0x107820 appends the
  animations) and the streaming loader 0x16C450; 0x117F30 (load) and 0x1186C0 (each frame) write each slot's pixel
  constant from its kind and colour (they read nothing else of the object); a draw (0x118B90) names its slot in byte
  +6 of its record; 0x1BEFE0 and 0x1073C0 reset. Paris has 113 slots: the extra overwrote material types 0 to 6 (the
  missing floor: type 5's stride 0 - 4 = -4), its kinds landed on the animated textures (a frame asked for stream
  block 7742: "m_CompressionStream.msg") and the per-frame pass wrote constants over the colours. The title update
  made the arrays 150 long. src/game/city_tables.cpp keeps the slots past 100 aside and the count at 100.
- **[PGR2]** The title update's default.xbe (in the DLC's uPAL folder) has the disc build's code with addresses moved;
  .local/tu has a Ghidra project of it (pgr2tu, .local/tu/g.sh runs the helper scripts on it). Aligning both builds'
  disassembly and pairing their globals shows what it enlarged: the city material arrays (100 to 150), the paint
  colours (600 to 700, 84 bytes each from 0x32F1F4), two lists beside the car table (0x310454 and 0x310888, +0x50
  each) and 0x20 bytes in the audio manager 0x465638 (its fields from +0x2CA0 move). The city streaming code (blocks
  0x3F2D38, unpacked in place by 0x1003E0) and its constants are the same.
- **[PGR2]** What the disc build cannot take from the booster packs (title update data): object paks in the newer layout
  (a zlib compressed WMSH chunk first, where the disc's open with an INDX index: read as garbage meshes), materials
  whose bucket is past the city draw list's 106 (0x11B210, table 0x457188: a null write), and night or overcast sets
  (the packs hold the day set). PGR2 draws an object it cannot set up with its error material, pixel constant 0 magenta.
- **[PGR2]** Paks: the disc's layout opens with INDX (tag, 0, offset of END - 12, count), then 16 byte entries (tag, 0,
  size, offset - 12), each chunk's data 16 byte aligned, the file ending with END's header (END, 0, 0). The title
  update's has each chunk inline (tag, 0, size with 0x80000000 compressed and 0x40000000, data; compressed data a 4 byte
  size and the game's own deflate stream). The chunks are byte for byte the same, so the layout converts losslessly
  (ConvertPak in src/game/dlc.cpp; the boats and the 18 DLC car models).
- **[PGR2]** The title update's 16 cars (its folder: cars.ini, CarColours.ini, Classes.ini, Manufacturers.ini, each
  car's dcontentcarN.pak_hrd/_opn/_sft, dcontentcarN.ini handling, <AudioData>.dat/.xwb) load as a disc car pack
  (category 1, 0x157180) once the models are converted. The INI loader's tables (handler 0x14DD80 by section type)
  are fixed size: cars 0x3109F0 cap 200 (0x270 bytes each from 0x3109F4), colours 0x32F1F0 cap 600 (0x14A8C0; past it
  a null write at 0x14E07C), windscreens 0x32F174 cap 10, classes (0x3103EC, 0x38 each) max 20, type 8 0x310884
  max 45 (45 with the pack loaded; what it lists not yet known). The disc has 102 cars and 566 colours; the update's 48 colours do not fit.
  Each player's car is 0x39B318 + player x 0x2498 (read by 0x17C50).
- **[PGR2]** A fatal error goes through 0x1388F0(message): it formats the cache partition and stops; look for its
  caller (Audio::Manager::Setup when a city has no stereo.ini line).

## System link

- **[console]** XNetGetConnectStatus must answer CONNECTED. The library's own version answers LOST when its network
  object is null, so PGR2 flagged the host as leaving and dropped it ("lost connection").
- **[console]** Loopback instances on Linux shift ports below 1024 up 20000 (PGR2 binds 1000 to 1002).
- **[PGR2]** The lobby, the race and the results work with two headless instances (`scripts/linktest.sh`).

- **[PGR2]** Clear-weather Nurburgring: the race's frame buffer clear is depth and stencil only (flags 3), so the sky
  dome (vertex shader with 1351-index strip, pixel shader blending a 256x128 gradient and a cloud texture) has to
  cover every pixel the scenery leaves. It and the far hills lie past the far plane; the console's W-buffer does not
  clip there, the GPU renderers did: old frames showed through (a white sky, brown bars that flashed while driving).
  Since sdk (W-buffered draws keep z within w) the vertex shaders clamp instead. The DLC TVR Cerbera Speed 12's Car
  Challenge is on "Nurburgring 1" by the title update's cars.ini.

## Sound

- **[PGR2]** The music volume is the float 0x367528 (0 to 1; Instant Action from the title runs at the default 0.7,
  a player's game at their setting). The menus' music carries it in its mix bin levels (-577 mB full, -1179 half);
  in races the radio goes to effect send 14 alone at 0 mB, its samples at full scale, and the DSP image applies the
  volume. The mixer plays such sends into the speakers itself, so GameInfo::send_level gives it the volume.

- **[console]** The software mixer plays everything PGR2 uses: streams, ADPCM, the engine via SetFrequency, and
  the music fades.
- Not modelled yet: the DSP effects image (the engine distortion). I3DL2 reverb: on the host since 2026-11-07
  (`audio/reverb.cpp`, fed by mix bin 10, following the listener's parameters rather than the DSP code). Filters: the low pass is (2026-11-07, `SetFilter` in dsound.cpp),
  the parametric EQ section is not (PGR2 set gain 0x2665 at EQ FC 0xB545, about 300 Hz, Q 1, on two voices; logged). What PGR2 asks of them (2026-11-07,
  headless races with the calls logged; plan in PROGRESS.md):
  - **[PGR2]** Doppler and velocity are never called (IDirectSound/Buffer/Stream SetDopplerFactor, SetVelocity), nor
    I3DL2Source, SetEG or SetLFO. EnableHeadphones(0) only. Panning is the title's own speaker bin levels.
  - **[PGR2]** Effects image `D:\data\audio\6WayDistortion.bin` (XAudioDownloadEffectsImage), 15 effects, no names:
    0-7 alike (code 0x154, state 0x9C bytes), 8 small, 9 large (0x5CC60 bytes of delay memory: the I3DL2 reverb),
    10, and 11-14 alike (0xA00 bytes of delay each); 8 and 10-14 never get SetEffectData.
  - **[PGR2]** Engines: each car's voice sends to its own FXSEND, mix bins 11-16 at 0 mB (speaker bins often -10000,
    so today they play dry through the send fallback), into distortion effects 0-7. SetEffectData(effect, offset 28,
    44 bytes) many times a second: 11 DSP 24 bit fractions. Word 0 a per-car constant (0x2DD2F1 to 0x40C878), word 1
    from 0x400000 up with the engine, words 2-5 a filter's coefficients moving with word 1, word 6 0x400000, words
    7-10 a fixed filter (0x807B8A 0x3FCA81 0x7F8475 0xC0357E). Read as: filter, drive, filter. The DSP code is not
    yet read, so the exact curve is a guess. The effects' states (image, 7 dword header: scratch offset and size, Y
    memory offset and size, flags, then input and output pointers) give the routing: an input at X:0x1400 + bin x 0x20,
    so effects 0-7 read bins 11-18, 9 (reverb) bins 10 and 20, 10 (cross talk) bins 6-9 into the speaker buffers
    X:0xC00-0xCA0, 8 bins 23 and 24 (the radio). Effects 0-7 write X:0xD60-0xE40, which no effect in the image reads,
    so how and how loud the distortion reaches the speakers is not known. The 11 parameters fit gain + two biquads
    (b0 b1 b2 -a1 -a2, halved): unused effects hold the same filter top and bottom (flat), and PGR2's pre filter is a
    peak that follows the engine (about +3.5 dB at 800 Hz to +6 dB at 220 Hz). The effect code segments are not DSP56300
    words as stored (top bytes random): scrambled; the library's upload (0x1EB7DC) would show how. Engine voices send
    to their bin at 0 mB while their speaker bins are -1000 mB and below: adding the distortion path at full level
    (tanh drive, 2026-11-07) made a race 5-10x louder and clipped, so it was taken out.
  - **[PGR2]** Reverb: mix bin 10 is the reverb send (about 200 voice setups, mostly -2000 to -3000 mB).
    IDirectSound_SetI3DL2Listener from 0x12E810 and 0x12EB35 with presets at 0x28F788 (48 bytes, DSI3DL2LISTENER):
    0 off (room -10000), 1 (room -1000, decay 1.49 s, HF ratio 0.54, reflections -2560 mB at 0.162 s, reverb -613 mB
    at 0.088 s, diffusion 79), 2 (0.4 s), 3 (1.65 s, HF ratio 2.0), 4 (2.1 s, reflections +1000, reverb +2000,
    diffusion 12, HF reference 1000). Pairs set at 0x12B21F (4, 4: menus), 0x12B827 (1, 4: races) and 0x12BFDF
    (a variable one and 3); 0x12EB35 blends the pair by [obj+0x30] and writes room as 0. Route of the race pad: preset 1
    throughout the race, 4 (reflections and reverb at 0 mB) in the menus; what moves the blend is not traced.
  - **[PGR2]** Filters: IDirectSoundBuffer_SetFilter from about 8 call sites (0x123416 to 0x1306C0), tens of
    thousands of times in a race; streams almost always bypass. Modes 0 (bypass), 1 (DLS2 low pass) and 3 (multi).
    The title builds them itself (0x126F80, 0x127110): cutoff FC = round(4096 x log2(2 sin(pi f / 48000))) & 0xFFFF
    for 30 to 8000 Hz (0x8000 below, 0 above, 0x126E40); resonance K = min(32768 x 10^(-Q/20), 0xFFFF), Q in dB up
    to 22.5 (0x126EA0); the parametric section's gain 16383 x g (g >= 0) or 0xC000 - 8192 x g. Seen cutoffs: about
    110 Hz to 4.8 kHz.
- **[PGR2]** The in-race radio (WMA from `data\audio\Radio\tracks`, decoded by the game's own WMADEC code into two
  44.1 kHz stereo PCM streams) goes to the effects send FXSEND 14 (mix bin 24) alone, every speaker bin at -10000: the
  DSP image plays it into the speakers (the in-car sound). With the effects not modelled it was silent in every build
  (Harry, 2026-10-23). A voice with no audible speaker bin now plays its effect sends into the speakers (dry, stereo
  kept); `XBE_DSOUND_LOG=2` lists every stream holding packets with its bins.

## Lessons on working

- Game code registers through an explicit call from `GameInfo::init`, never a static initializer: the Windows exe
  dropped one.
- Leftover environment variables from earlier experiments cause false regressions. The log now lists every `XBE_*`
  and `PGR2_*` variable at startup.
- Captures flush the GPU every frame and never caught a timing-dependent flicker. The frame log (`XBE_FRAMELOG`, F4)
  records at full speed and found the one-frame state changes.
- The CPU reference renderer and the D3D12 backend are written separately. A fault in both (the showroom depth) is
  in shared state handling; a fault in one only is in that renderer.
- When a draw writes nothing, check the inputs before the shaders: vertex format (SetVertexShaderInput), constant
  order (pushed vs direct), target and depth sizes, and the depth mode.

## Still open

- Any remaining flicker on Windows (the Edinburgh night buildings and the under-car patch are fixed).
- The CPU reference renderer's random spike pattern in single snaps.
- START during the second intro movie hung the player earlier. It now skips, but this was not root-caused.
- Sound effects (reverb, filters) are not modelled.
