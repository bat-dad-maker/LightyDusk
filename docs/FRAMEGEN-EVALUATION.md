# Frame generation in BOTWRecompAndroid: evaluation (23/10/2026)

Question: Is the LASRecompAndroid framegen for BOTW? Abstract: **serve as option "30 → 60" for GPUs that do not hold 60 (Adreno 7xx, Mali)**. It's not the way to Snapdragon 8 Elite, where the goal is still 60 for real. Nothing's been ported yet. This note tells you what porting requires.

## What LAS framegen does

`LASRecompAndroid/runtime/nvn/vulkan/framegen.c` (~950 lines, MIT, from the LASRecomp-Android-Plus of Nycolazs, which came from the `reproj.cpp` of the Wind Waker HD port):

- The game runs at 30. After each real frame, the GPU makes an intermediate frame, presented a later update (FIFO).
- The intermediate is the actual frame **redesigned with the depth of the scene**: the camera is advanced half step, with the view-projection of the current frame and the previous one.
- **HUD:** the image is copied at the moment the scene is composed in the final image before the HUD. The pixels in which the final image differs from that copy are HUD and stand still.
- **Link** is where the real frame has it (radio around its position on the screen). The other characters only move at 30 per second. The exception is `LARECOMP_FRAMEGEN_FLOW=1`, which estimates movement by blocks and is experimental.
- **Overlays of the window** (touch controls, FPS counter) are drawn again over the middle.

## What BOTW already has and what is missing

| Entrada do framegen | No LAS | No BOTW hoje |
| --- | --- | --- |
| Moment of scene composition (before HUD) | `scene_composite` in exec.c | **There**: `scene_upscale` already finds this draw (FSR 1 / SGSR 1 of the scene). |
| Depth of the scene | `fg_note_draw` no passe principal | **There**: G-buffer pass (6 targets + depth, 1600x900 or dynamic resolution). Just write it down. |
| View-projection of the camera, per frame | `widescreen.c` reads scene UBO (320 bytes, matrix in +32) | ** Lack**: `widescreen_scene_ubo` is a stub in BOTW. You need to find the constant buffer on the AGL camera. The parameters have already been seen in the heap (near 1, far 25000, fovy 0.8727, aspect 1.7778). |
| Link Position | `doorjump_link_pos` (Link's Awakening) | ** Lack**: read the player's actor's position on 1.8.2 (`ksys::act::PlayerInfo`, there are names on decomp zeldaret). |
| Ritmo 30 + 30 | o LAS roda a 30 | **There**: `framepace.c` fixed 30 and the game clock (VFR) accompanies. |

## Expected quality in BOTW style

- ** Good:** Turn and camera displacement in open field, the most common case. Deeply reprojected handles this well, and the grass with alpha-test is in the depth buffer.
- **Fraco:**
  - Particles, water, smoke, fog and glass are not deep and "grabbing" along with whatever's behind.
  - Borders of moving objects open holes (disocclusion), filled with the neighbor.
  - Enemies and walking NPCs stay at 30 (judder), as in LAS without the flow mode.
  - Latency: about +1 table (16.7 ms).
- The HUD of BOTW has many animations (follego, heart, minimap). The mask for difference already covers it in LAS.

## Custo

- The LAS measures a warp pass in the output resolution, plus a preparation and the mask per real frame. At BOTW the output is 2560x1440 at the POCO.
- Estimate: 1 to 2 ms of GPU per real frame. In a GPU that already makes 30 with clearance (Adreno 7xx to 1x), fits.
- In the 8 Elite at 60 does not help: there the problem is to hold 16.6 ms, and the framegen only enters when the game runs at 30.

## Recommendation and steps

It is worth porting as option **Video > Frame Generation (30 → 60)**, turned off by default. That's after 60 fps native on the 8 Elite.

1. Find the camera UBO:
   - On a debug board, dump the UBOs from G-buffer's draws.
   - Find the matrix whose product matches the known projection (aspect 1.7778, near 1, far 25000).
   - Connect `widescreen_scene_ubo` to her. This also helps the widescreen, which looks for the same camera.
2. Find Link's position on 1.8.2. It's a memory reading per frame.
3. Port `framegen.c` and hooks in exec.c and device.c:
   - `fg_note_draw` in G-buffer pass;
   - `fg_scene_composited` in `scene_upscale`;
   - in device.c, the extra frames in the presentation.
4. Test in open field, in sanctuary (interior) and with rain and particles. Measure the cost with `LARECOMP_GPUPROF_AT`.

Estimated effort: one to two days. Most of it goes to items 1 and 2 and visual testing.
