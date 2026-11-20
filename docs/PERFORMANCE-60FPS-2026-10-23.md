# Towards 60 fps in POCO F7 Ultra (Turnip): measurements of 23/10/2026

Scene: 1.8.2, Great Plateau near the Temple of Time (test save), Video > Frame rate **60**, internal resolution 1x. Apparatus: Adreno 830, Turnip Table 26.3-devel.

## How to measure without being mistaken

- **Use `frame_rate = 60` in the test file.**
  - With `frame_rate = 30` (app default), the pacer pretends slow GPU counters.
  - The game then stays in the minimal dynamic resolution, even with `LARECOMP_FPS=60`.
  - The tests from before tonight were in this mixed mode.
- **The cell phone heats up (60–90 °C) in a few minutes and, loading, does not cool below ~50 °C.**
  - Two straight rounds differ more than most changes.
  - Use ** `LARECOMP_AB=<quadros>:<chave>` **: switches a key to each N frames within the same round, and the `[pace]` log marks `ab 0/1`. Both sides are in the same heat.
  - Chaves: `split`, `ubo`, `sgsr`, `nobarrier`, `copy`, `chunk`.
- ** `LARECOMP_GPUPROF_AT=<quadro>`:** GPU time per pass. Absolute values are inflated by timestamps, but proportions are worth it.
- ** `simpleperf` ** works on the app (debuggable). Symbols: the `libmain.so` of `merged_native_libs` (-O2 -g).

## What's changed and how much you've earned

Measurements in the same round (A/B):

| Change | Before | Later. |
| --- | --- | --- |
| Read in place instead of feedback copy (9d0ea24, 1725400) | 31,2 fps, GPU 22,4 ms | **40,5 fps, GPU 16,6 ms** |
| SGSR 1 instead of FSR 1 (new standard on Android, 6a67bd8) | 35,9 fps, GPU 19,7 ms | **39,0 fps, GPU 18,3 ms** |
| Constant buffers lidos no lugar (`LARECOMP_UBO_DIRECT=1`) | render 13,0 ms | render 12,4 ms (GPU igual) |

- ** Feedback copies:** the ground decals (fs `f984e489`) read and write two G-buffer targets, with a barrier to each one or two draws. There were 32 pairs of full-screen copies per frame, ~14 ms of GPU. Now the pass only ends where there was a copy, and the draw reads the target itself. It's a loop feedback that the Vulkan leaves undefined without `VK_EXT_attachment_feedback_loop_layout`, so it's only on Turnip**, where it was given frame by frame. The other drivers keep the copies, or use `LARECOMP_FEEDBACK_INPLACE=1`.
- ** Black traces on the grass (b9321e7):** were from FSR 1. The intermediate output A2B10G10R10 compressed (UBWC) returned blocks 16x4 of zeros to the RCAS in Turnip. Now this image is created without compression. Before, it was ~1 per frame.

## No effect tests (the patterns stay)

- ** 50 % resolution:** equal GPU (~18 ms). The cost is not per pixel.
- ** Final output in 1080p instead of 1440p** (`LARECOMP_OUT_LINES=1080`): ≤ 1 ms.
- **Bilinear instead of SGSR:** within the noise.
- ** `TU_DEBUG=sysmem`:** equals autotune. ** `TU_DEBUG=gmem`:** much worse (GPU ~30 ms).
- ** 32-bit shader mathematics** (`LARECOMP_VK_INT32=1`): equal to emulated int64.
- **`IR3_SHADER_DEBUG=nouboopt`:** igual.
- ** Without the barrier at the beginning of each pass** (`nobarrier`): equal.
- **Without splitting the long pass** (`split`): ~0.5 ms less GPU, but the frame gets longer because the GPU starts later.
- **Submissions every 0.5 ms** (`chunk`): equal.
- ** Shared window for the constant buffers of 64 KB:** worse. They are always the same address, with 16 bytes rewritten before each draw (+0x70). Each bind needs its own copy, ~14 MB per memcpy frame.

## Where's the limit now?

- **The game awaits the fence of the frame before each frame** (`LARECOMP_LOG_SYNC`, lr `100d81418`).
  - It is 8.5–9 ms waiting for the GPU; the translation has passed that point.
  - The GPU (~17 ms work, plus ~3 ms stop at the beginning of each frame) is what holds the game at ~40 fps.
  - The screen is 120 Hz: between 16.7 and 25 ms, the frame falls by 25 ms.
- **For 60 stable here, GPU work needs to stay at ~13–14 ms**, ~25% less. The cost is by draw (~2 μs) and pass (~20–40 μs; are ~190 passes per frame), not by pixel.
- **Thread of render:** ~12–13 ms per frame. It's not the limit today, but it's close. Largest items in `simpleperf`:
  - `draw_` 13%;
  - descriptor sets 12%;
  - memcpy of constant buffers 12%;
  - `begin_pass` 9%.

## Widescreen pass profile (late 23/10)

Great Plato, mode 60, widescreen 3200x1440, scene 1x (1600x720), `LARECOMP_GPUPROF_AT=1700`. With time marks the frame measures ~26 ms of passes (more than ~17 ms without them):

| Item | Tempo | Observation |
| --- | --- | --- |
| G-buffer (6 cores + profundidade) | 10,4 ms | 55 passes, ~2.460 draws; the bulk of the frame |
| 1 color passes in 1920x1080 (coach to 3200x1440) | ~3,4 ms | final composition; the last includes the GPU stopped until the next submission |
| Upscale of the scene (SGSR 1, 1600x720 → 3200x1440) | 2,0 ms | |
| Shadows (deepness only) | 1,7 ms | 11 passes, 526 draws |
| Decalques lidos no lugar (`f984e489`, `69c0a04e`) | ~0,9 ms | ~40 passes of 1 draw, ~17 μs each (sysmem) |

- The new counter on the fps log (`fbcopy... breaks N`) shows ~80 frame-pass breaks for readings in place.
- **Conclusion:** take out these breaks would gain less than 1 ms (~3%). It does not compensate for the risk of a loop feedback within the pass. Time lies in G-buffer geometry, upscale and output resolution composition.

## Compatibility with other GPUs (23/10)

Safe paths (memory of the game by copies of the CPU, copies instead of reading in place, LRZ turned off), simulating a GPU that is not Adreno 8xx: the world draws right, but the **15 fps** against 35 (5,5 GB/s of copies). That's why the Automated Driver Compatibility only uses the safe paths outside the 8xx Adreno.

## Next possible steps

1. ** Smaller output resolution on the widescreen:** the final composition and the upscale run at 3200x1440; a smaller output option (with less sharp HUD) would cut off part of ~5 ms. The decal passes are not worth it.
2. **Measure the GPU clock** (Turnip `VK_KHR_performance_query` counters) to separate governor's work from frequency.
3. **Chart generation (30 → 60)** for GPUs that do not reach 60: see `FRAMEGEN-EVALUATION.md`.

## 26/10: Automatic output, stopped GPU and the render thread

Same cell phone, 1.8.2 on the Great Plateau, one player's settings (50%, FSR 1, Output: Screen, 3200x1440).

- ** Automatic output (60ac287):** the final image in 1080 lines on larger screens; the scene keeps its size. GPU 22,8 → ~17.6 ms per frame. The step of presenting was bilinear (0.13 ms; the SGSR 1 there cost 1.9 ms without visible difference in 1.33x).
- ** Time line of a frame (`LARECOMP_FRAME_TIMELINE`, 6a59793):**
  - the game simulates ~16 ms;
  - wait ~4–6 ms the fence of the table that **just send**;
  - records and sends the list of the frame at ~2.6 ms, with the GPU stopped;
  - the rendering thread translates into ~11–13 ms, subjecting every 2–3 ms;
  - GPU ends ~9 ms after last submission.
- **GPU stop (`LARECOMP_LOG_GPU_IDLE`):**~4 ms per frame within the frame, waiting for translation, and ~2.7 ms between frames, waiting for the game to record.
- **Render thread is on the critical path, but the profile is flat:**
  - ~2.400 draws per frame, ~6 μs each;
  - the largest item is the constant buffers dynamic memcpy (~10%): ~16 MB per frame, ~22 KB each, because the game rewrites 16 bytes before each draw.

### Tests without effect (26/10)

- **Micro-optimizations of translation (be08e10):** `draw_writes` once per draw, hash of the key in words, `AB()` without call. Surrender 14,85 → 14,62 ms, within the noise. The "hot loop" of `draw_writes` in simpleperf was a sampling slide.
- **Thread copies** (copies ≥ 2 KB of constant buffers in other core, order):
  - pending copies receive a patch with the old bytes when the game writes over, and the queue emptys before each submission;
  - the image is right, and ~12 MB per frame goes through the thread;
  - the render thread waits ~0.3–1 ms per frame on the barrier;
  - A/B in the same round: render 13.52 ms without, 13.82 com. Reverse.
- **Set the main thread of the game into a prime core:** already tested at 16/10 (`LARECOMP_PIN`). Takes down the game, for a race in the job system.

### ♪ Where there's still a win ♪

1. ** The game a frame ahead** (liberate the fence before the GPU ends). Eliminate ~2.7 ms between frames and override the GPU translation from the previous table. The risk is the game rewrite vertex data read in place while the GPU still uses them.
2. **The 16 bytes per draw as push constants:** the shadow translator would read this excerpt from the constant buffer from elsewhere, and the 64 KB copies per draw would end.
3. **The passes of the scene in the GPU** (the G-buffer, ~10 ms).
4. ** The simulation of the game (~16 ms per frame):** alone, already limits to ~53 fps in this scene.

### More 26/10 tests (Optimizations List and SavageWindRecomp)

- ** `-O3` + ThinLTO in libmain:** worst. 31.8 and 30.5 fps against 34.6 and 35.5 with `-O2`, in two rounds of each. Probably by the larger code in the instruction cache. That's `-O2`.
- ** `SleepThread(0)` sleeping 50 μs after 32 yelds followed:**
  - +1% of fps, within noise;
  - Process CPU 2.51 → 2.47 cores, no temperature difference;
  - the spin with `sched_yield` costs less than 3.3% of the profile suggested. He didn't.
- **Texture consulted once per draw:**The duplicate part is ~0.5% of the yield thread, below which a measurement separates. He didn't.
- ** Cheaper SGSL from SavageWindRecomp (9f94aba):** build-ins in conversions, only the prelude functions that the shadower uses and SPIR-V without names. With cold caches:
  - wait for shaders 17,3 → 11,2 s;
  - worst picture 6.2 → 3.8 s;
  - The same image.
- **Commory of commands (#74 SavageWindRecomp):** does not apply here.
  - After 2 min in the world, 43 shadow pages (21 MB address, minus RSS) in a 2.2 GB process.
  - BOTW pools have 16 KB, less than a 64 KB shadow page, so releasing entire pages when a pool closes did not release any in ~300 submissions.
  - The shadow here is by memory page of the game and is reused when the game reuses memory. Reverse.
