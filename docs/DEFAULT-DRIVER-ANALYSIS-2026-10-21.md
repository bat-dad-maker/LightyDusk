# Qualcomm Standard Driver: Analysis and Possible Solutions

Date: 21/10/2026. Scope: analyze the current version and prepare future work without implementing corrections. Cell phone execution was authorized during analysis.

**Later update, October 21, 2026:** the user authorized to correct the two shaders and test on the mobile phone. This step has been completed; see the final section. The rest of the report preserves the analysis prior to correction.

## Conclusion

The visual defect has been reproduced with the standard driver: Link appears almost entirely black; terrain and trees lose their normal appearance, while sky, interface and minimap remain visible. Capture does not allow you to determine alone whether there are no textures of materials or lighting data.

The strongest evidence of this execution is the failure of **two computing pipelines**, accompanied by continuously skipped commands after completion of the compilation. The hypothesis of general failure in loading textures was not confirmed: there were no incompatible format messages or image creation failure in the collection. This does not prove that all uploads and descriptors are correct.

Recommended priority: identify and correct the two failing shaders; check the data that feeds the lighting; only then deepen formats, uploads and performance. The lack of shared memory between the game and the GPU makes it necessary to have a correct copy path, but does not make correct rendering impossible.

## Version and method

- Project in `4fc0d281105ee737fea6c05a31d299e9853fca3a`, with pre-existing changes in workspace. The consulted graphical code already contains resident arena (`gres`) and address learning (`gmiss`); it should not be proposed to implement them from scratch.
- `runtime/nvn/vulkan/exec.c`: SHA-256 `23C8CF167EC94A88A65699DB707201D14AE6BCA06AA49AE08DFF6C0BE77ACFA8`.
- `runtime/nvn/vulkan/textures.c`: SHA-256 `F28820A1E7541D6A7E07606447043AA4F6967A738D68FA4B93DFCAC7FEDA9F49`.
- Cellular POCO F7 Ultra, model `24122RKC7G`, Adreno 830. APK installed informs `versionName=0.0.1`, `versionCode=1`, `2026-10-21 12:31:37` update according to Android. These numbers do not identify the binary commit alone; binary verification of equivalence with the source was not performed.
- The game was closed. The existing APK has been opened, without recompiling or installing, with `LARECOMP_NO_LAUNCHER=1`, `LARECOMP_VK_DRIVER=none`, `LARECOMP_VK_LIST_EXT=1`, `LARECOMP_GMISS=1` and a log file itself. The user loaded the save and prepared the scene.
- `none` is not a valid library: it causes explicit fallback for `libvulkan.so`. The log confirmed `using the system driver`, Adreno 830 and the specific path Qualcomm. It does not represent system driver boot failure.
- Permanent driver preference, code, APK or correction parameters were not changed. No comparative tests were performed with other options in this collection. The game was left open; the normal session can perform self-saves.
- The times include instrumentation `GMISS`, thermal status and device activity; they serve to characterize this test, not as a definitive benchmark against Turnip.

Local evidence, not intended to commit:

- [Scene capture](../build/driver-system-analysis-20261021/scene.png).
- [Execution log](../build/driver-system-analysis-20261021/runtime.log).
- Log original no celular: `/sdcard/Android/data/com.botwrecomp.android/files/driver-system-analysis-20261021.log`.

## Observed results

| Evidence | Result | Interpretation and limit |
| --- | --- | --- |
| Driver and memory | Qualcomm; `no heap import`; arena de 128 MB iniciada | The copy path and arena is active; it is not the heap placed directly into the GPU used with Turnip. |
| Compute | Duas mensagens `[compute] pipeline failed: -13` | Real creation flaws. `-13` is `VK_ERROR_UNKNOWN`; it does not identify the cause or visual function of these shaders. |
| Graphical compilation | No graphical pipeline failed message located | Do not confuse computer failure with failure of the entire renderer. |
| Formatos e imagens | No messages from `not supported: RGBA8`, `image creation failed` or `no BC textures` | There was no positive evidence of these fallbacks in this collection. |
| Stable sample, t=80–110 s | 15 lines of statistics; simple average of 16.1 FPS, range 14.6–17.9; zero new compilations | Visual failures and command losses persist with stable compilation. |
| Commands jumped on the same sample | 210–252 per range of approximately 2 s | The `stat_skipped` counter includes computer disks; they are not necessarily pulped texture designs. |
| Volume counted as upload | Average 4.022 MB per interval of approximately 2 s | Allocation/copy counter on the transitory path; it does not mean 4 GB of new textures, nor directly measures physical RAM traffic. |
| Accesses outside the table | Up to 94,189 accesses in a report; last record of the copy collected: 903 in frame 1604 | `gmiss` returns zero in these accesses and learns regions for next frames. It's not proof of nonexistent texture. |
| Arena no frame 2400 | 23 living regions, 126 entries, 6.1/32 MB residents, 10 learned regions | No fallback due to lack of space, full table, ring or excessive size in the collected reports. Saturation is a risk of robustness, not a cause demonstrated in this scene. |

After frame 1604 no new mises reports appear in the copied log, but the capture remains defective. This reduces the strength of the hypothesis that just increasing the coverage learned would solve the image. Learning is sampled and limited, so no new messages also do not certify all accesses.

## Lista priorizada de trabalho

### 1. Identify and correct the two computing pipelines — top priority

** Evidence:** fail in this execution, and `dispatch_compute()` abandons command when pipeline is not ready or failed. The current log shows only the result, no shader address/hash that failed.

**Proposed work:**Enrich the diagnosis of `build_compute()` with address/hash, local dimensions, bindings and capabilities used; store GLSL/SPIR-V in private folder; validate SPIR-V and device limits. Play each pipeline on Qualcomm alone, comparing with Turnip only as control. Investigate control flow, 64-bit types/operations, group size specialization, and resource pressure only according to the identified shader.

**Possible solution:** adjust Maxwell→GLSL/SPIR-V translation to default that triggers failure, with specific compatibility variant when needed. Do not treat `-13` code as a diagnosis of memory or texture lack.

**Accept:** both pipelines are created, their dispatches run, the output contains the expected data and the scene improves. If there is no visual improvement, maintain the execution correction and investigate the next step; it is not yet proven that both produce illumination.

** Local references:** `runtime/nvn/vulkan/exec.c`, `build_compute()` (~3920), `compute_program()` (~3947), `dispatch_compute()` (~3990); `runtime/nvn/maxwell/translate.c`.

### 2. Validate data continuity between computer and drawing — high priority

**evidence:** the history documents the list of lights read before the result of the computer returns to the heap. The current code already tries to solve this by keeping regions in the GPU arena. There were real mises and learning in the test.

**Proposed work:** to accompany, for a frame and a consumer shader, the guest address, resident slot, range, version, light counter and content before/after the producer. Include indirect pointers and headers prior to storage buffer. Compare the same data consumed in the GPU, not just the delayed copy in the CPU.

**Possible solution:** preserve a coherent representation by interval between all producers and consumers; expand mapping as needed accesses and avoid relying on various learning frameworks for correct reading. Also evaluate data used as uniforms, vertices or texture sources, which can follow another copy path.

**Accept:** the consumer reads the producer's current output in the same sequence of commands; the scene remains correct when walking and loading other areas. The arena already exists and did not present saturation in this execution.

**References:** `gcopy_prepare()` (~3401), `gres_prepare()` (~3230), `gres_learn()` (~3202), `gcopy_retire()` (~3021), `stage_set()`; `translate.c`, generation of `GMap` / `gmiss` (~2524).

### 3. Audit synchronization and CPU/GPU property — high priority

**Static evidence:** `gres_sync()` checks changes once per frame and places copies in the upload command buffer, run before the main buffer. `gcopy_retire()` reconciles changes in 256 byte blocks. `GMISS` is read/zeroed by CPU in the mapped buffer; you need to check your relationship with submissions still in flight.

**Proposed work:** check CPU writings within the same frame, sorting uploads against computers, overlaying regions and reading CPU results from GPU. Record real properties of the types of memory of the arena and ring. In these paths, the code does not explicitly require `HOST_COHERENT` and does not present local flush/invalidate calls corresponding to each access.

**Possible solution:** Track versions by region/submission, use barriers and waits in real dependencies, use readback buffers protected by GPU completion and flush/invalidate when the chosen memory is not coherent. A GPU barrier does not recover copied CPU data before it is ready. Coherent memory also does not eliminate the need to order competing accesses.

** Limit:** the project already calls `vk_full_barrier()` in important transitions. There is no proof that simply adding a barrier after each draw solves the defect. Memory type coherence still needs specific measurement.

**Accept:** no early or overwritten reading of valid results; clean synchronization validation and stable result with split and normal submissions.

### 4. Fix texture fallbacks that discard content — medium, conditional priority

** Concrete static evidence:** `textures.c` can exchange a format not supported by RGBA8. In `upload_texture()`, `uploaded=true` and hash are updated before the return that discards formats without BC/ASTC converter. Thus, there is a path that logs upload without sending Texels. There was no message to prove this path in the current test.

**Proposed work:** count effective uploads, conversions and returns without sending by format/texture; consult support for the complete combination of format, use, dimensions, flags, samples and image type. Capture albedo, normal and intermediate targets to distinguish lack of black lighting material.

**Possible solution:** implement correct conversion to the format effectively found; mark upload completed only after successful preparation of commands; validate separately views, color space, swizzle, mips and layers. Avoid using RGBA8 as a universal depth substitute, integers or other incompatible formats.

**Accept:** the sent and sampled Texels correspond to the expected content. `LARECOMP_NO_BC=1` may isolate the BC decoder in a future test, but it is not a confirmed correction nor was it used here.

**References:** `textures.c`, creation (~374–440), `upload_texture()` (~632–690), `vk_view()`.

### 5. Ruggedness of the arena and tables — average priority

**Static evidence:** Resident arena limited to 32 MB/512 entries; `GMAP_MAX=16` table; `GL_MAX=16` learning; `gc_back[256]` return queue. Some failures return to simple copies; `gcopy_prepare()` leaves the table empty if the total surpasses `CHUNK_SIZE=64 MB`, producing zero readings. The return log can also stop occurring when the queue is full.

**Possible solution:** instrument all limits, control reuse at the end of the GPU, divide intervals/tables without losing aliases and ensure a correct fallback. Avoid joining distant regions into a huge copy only to fit into 16 entries. Audit queue cleaning and counters on internal reboot.

**Accept:**long sessions and area exchange do not cause silent zero readings, growth without recovery or loss of results. Do not increase constants indiscriminately: the accountants of the current scene did not accuse these limits.

### 6. Cache, views and texture update — later priority

**Static evidence:** Uploads use frame verification, sampled hash, periodic scan and view memoization. The device confirmed the absence of page writing screening. Changes outside the sample may require further detection; GPU writing needs to be treated separately from CPU writing.

** Possible solution:** invalidate by content/generation and sub-resource when known; compare individual tests with `LARECOMP_TEX_FULLHASH=1`, `LARECOMP_VK_NO_TEX_MEMO=1` and `LARECOMP_NO_SET_CACHE=1`. Check aliases, re-use of descriptors, and feedback to render targets. Don't run all the options together, because that prevents us from identifying the cause.

**Accept:** Textures and targets change in the correct frame, without flicker or regression in Turnip. These tests were not performed in this analysis.

### 7. Reduce copy cost after recovering image — performance priority

**evidence:** approximately 4 GB counted in uploads per 2 s interval, with about 16 FPS in this scene. The arena itself did not eliminate that volume.

**Possible solution:** Separate statistics from uniforms, vertices, indexes, global memory and texels; reuse immutable data or without changes with correct invalidation; copy only modified regions. Measure render CPU, GPU, waits and presentation separately. Repeat the measurement without diagnosis after stabilizing the image.

**Accept:**Measurable reduction of bytes and time per frame, maintaining visual equivalence and shader results. Do not promise 60 FPS: CPU/GPU budget and game cadence are additional problems.

## Ways already tried that are not the first choice

The [BOTW-PORT.md](BOTW-PORT.md) history, Qualcomm driver measurement section, records undone attempts at `vkMapMemory` + `mremap`, import/export `OPAQUE_FD` and AHardwareBuffer BLOB. They failed the device. They weren't repeated here.

Reopening these alternatives requires a concrete difference of support or implementation and a minimal isolated case. Forcing a mapping at the address of the heap can compromise the memory of the game. The priority is to make correct the copy path already available. The fact that an external extension exists does not guarantee the combination of import, format and use required.

## Suggested sequence for future implementation

1. Instrument and isolate the two computers that return `-13`.
2. Correct them and compare the image in the same scene.
3. If the lighting remains incorrect, track your data between producer and consumer and audit sync/coherence.
4. If the data is correct, check intermediate images, uploads, formats and descriptors.
5. Validate movement, inside/outside, internal loading and reboot; then optimize copies.

A future test with `LARECOMP_SAVE_MSL=1` may save GLSL, but the error diagnosis still needs to associate each flaw with the shader. Game-derived files must remain private, as `CONTRIBUTING.md`.

## Technical sources consulted

- [Khronos: VkResult](https://docs.vulkan.org/refpages/latest/refpages/source/VkResult.html): `-13` corresponds to `VK_ERROR_UNKNOWN`; the number alone does not determine the cause.
- [Khronos: examples of synchronization](https://docs.vulkan.org/guide/latest/synchronization_examples.html): dependencies between producer and consumer must cover stages and accesses involved; CPU reading requires observing completion and visibility.
- [Vulkan Memory Allocator: Memory mapping](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/memory_mapping.html): mapped memory does not imply coherence; direct copies need appropriate cache treatment when the type is not `HOST_COHERENT`.

The above priorities combine evidence of this execution with code inspection. No proposed solution has been implemented or declared validated in this analysis.

## Update: Two pipelines fixed and tested on Qualcomm

After authorization for implementation, the instrumentation identified the two pipelines that returned `VK_ERROR_UNKNOWN`: addresses `36ec60000` and `36ee37000` in the execution of diagnosis. There were two instances of the same shader: identical GLSL, SHA-256 `d9da971da7bbbe7bb1bd76fa51106c6fbe5e9d86e25ad19c30c08f9ad68fd397`, local group `32×1×1`, ten bindings and 220,308 SPIR-V words. Addresses change between sessions.

The original SPIR-V passed in `spirv-val --target-env vulkan1.3`. An independent diagnostic program reproduced rejection on its own cell phone, using the system's Vulkan, the application's shaderc library and binding-derived layouts. This allowed testing creation without replacing shaders in the game. The device reported a compiler `E031.47.18.49` and `/vendor/lib64/hw/vulkan.adreno.so`.

| Variante isolada | Creation result |
| --- | --- |
| Original, binary cache search from the last region | `-13` |
| SPIR-V optimized with `spirv-opt -O` | `-13` |
| Shadow sampling removed, diagnostic only | `-13` |
| Fixed group size instead of specialist | `-13` |
| Control flow structured by the existing translator | `-13` |
| Linear cacheless search | `VK_SUCCESS` |
| Linear cache search from the last region | `VK_SUCCESS` |
| Binary search without cache | `VK_SUCCESS` |

The isolated trigger is the combination of binary search with cache in this large shadower; the internal defect of the Qualcomm compiler has not been identified. The correction uses the linear search with the same cache. It solves the same non-overlapping intervals of `GMap`, maintains the identification of mises and does not remove instructions, textures or calculations from the game.

### Amendments

- `runtime/nvn/maxwell/shader.h`: `ShaderKey.gmem_linear_lookup` option.
- `runtime/nvn/maxwell/translate.c`: linear query variant that preserves the cache of the last region.
- `runtime/nvn/vulkan/exec.c`: activates the variant only in the computer shaders of the Qualcomm proprietary driver on Android. Graphics and Turnip maintain the previous selection. The SPIR-V cache already depends on the generated text, so the user cache has not been deleted.
- Compute diagnosis now identifies address, result, local size, SPIR-V size and bindings. `LARECOMP_LOG_CS_PIPELINES=1` also records successes; with `LARECOMP_SAVE_MSL=1`, failures keep SPIR-V next to GLSL. The temporary Vulkan module is released after creating the pipeline.

### Validation in the game

- `assembleDebug --offline --console=plain`: success. A pre-existing warning of `const` disposal remains in `g_dump_sampled`, outside of this correction.
- APK installed on the phone and run with the default driver. The `save-shader-fix-20261021` copy was created and used, separate from normal saves.
- In the corrected session, the two instances appeared in `36eb8c000` and `36edef000`, both with `result 0`, `32×1×1`, ten bindings and 220,229 SPIR-V words.
- The two SPIR-V generated by the application were recovered from their cache and passed individually on `spirv-val --target-env vulkan1.3`.
- It also verified the textual equivalence between the corrected translator output and the variant accepted in the isolated test, disregarding only diagnostic comments.
- After finishing the compilation, the statistics started recording **zero skipped commands**, with hundreds of dyspatches executed by interval. In window t=84–96 s, approximately 11.5–17.7 FPS were observed. It is not a comparison of controlled performance: the device was warmer and now performs previously discarded works.
- The first compilation of some pipelines took tens of seconds; there is no promise of improvement in loading time.

** Limit of result:** the capture still shows Link as silhouette and the scenario without normal lighting. The two pipeline errors have been corrected; the complete visual failure remains open. The next priority is to track the lighting data between computer and drawing, including copy and synchronization paths. There was no change in these paths at this stage.

Private evidence in `build/shader-fix-20261021/`: `baseline.log`, `corrected.log`, `corrected.png`, `isolated-results.json`, Shader sources and binaries and diagnosis. Do not add dumps or game-derived shaders to Git. The session left on the phone uses the test saves.

## Update: cause of image without light found and corrected

Continuation of the above analysis, same date. Step 3 of the suggested sequence (trace the lighting data) was done first by deletion and then by comparing passes.

1. ** Copy mode is not the cause.** Turnip with shared heap off (`LARECOMP_VK_NO_PLACED=1`) falls in the same copy path and arena (`gcopy` / `gres`, ~6 GB counted every 2 s) and shows the scene illuminated normally. The defect is specific to the Qualcomm driver.
2. **Pass-pass comparison.**Debug table 2200 on the two drivers (`LARECOMP_DEBUG_FRAME=2200`, with `LARECOMP_VK_SPLIT_US=0` on Turnip for the passes to match), aligned with the programs of each pass. The G-buffer comes out the same in both, but the composition of light in Qualcomm has not albedo.
3. ** Cause.** BOTW G-buffer connects the same image into two color slots of the same pass: albedo (`f0x38`) on normal slots 1 and 2, (`f0x25`) on slots 3 and 4. The game writes only one slot of each pair (`f0f0f0ff` channel mask). In a tiller, each slot has its copy in the tile memory and the two are recorded at the end of the pass. In the Qualcomm driver, the masked slot copy, with the old content, arrives last and deletes the albedo. Result: Link in silhouette and scenery only with fog. In Vulkan, two attachments with the same image written in the same pass have resulted indefinite; Turnip only happens to hit the order by chance.
4. **Correction** (`exec.c`, `begin_pass`): when the same view appears in more than one slot, only the slot that the channel mask writes is turned on; the others are `VK_NULL_HANDLE`, with `UNDEFINED` format on the pipeline. If a later draw of the same pass type in a slot off, the pass starts again (`pass_dup_null`).
5. **Also corrected, with no visible effect on this scene:** a sampler with depth comparison used in an unparalleled reading (common `texture()`) is indefinite in Vulkan. Now these readings use an unparalleled sampler variant (`vk_sampler_plain`, `textures.c`). False positive on the way: the linear depth pass seemed zeroed only by a misalignment of a pass in the first comparison.
6. **New diagnosis:**In debugging frames, the directly sampled depth in the pass is saved as `build/fbdepth_NNN_inplace.pgm`. `debug_save_depth` accepted `D32_SFLOAT`.

**Validation on the mobile phone:** with the Qualcomm driver, the plateau scene is with normal albedo, light and Link, the ~13–15 fps (the copy mode remains slow). Turnip's still the same, 30 fps. No restart of the system registered in `dropbox` during the tests.

**Next step on Qualcomm driver:** performance. Surrender ~34 ms per frame, dominated by copies of copy mode (item 7 above). It is also worth checking other scenes (interiores, sanctuaries, water) with this driver.

## Update: Copy mode performance (writing tracking and vertices cache)

Profile of the render thread on the Qualcomm driver (simpleperf, plateau): ~37 ms per frame, 22% on `memcpy` coming from `guest_copy`. Measurement of copies: ~120 MB per frame, ~90 MB of this in **vertices and indexes** copied to each frame. In Turnip they are read directly from the heap (`guest_direct`); the vertices cache (`vc_*`) existed, but requires page writing tracking, which Android did not have.

- ** Screenplay on Android** (`runtime/platform/android.c`, only in copy mode without placed memory):
  - `plat_written_pages` leaves the pages consulted read only. The first writing of the game generates SIGSEGV; the handler marks the page as written and releases it.
  - It also covers a writing that crosses the border between two pages.
  - `pread` s of `fs` release target pages (`plat_write_prepare`), because a kernel writing does not generate a missing one, it returns EFAULT.
  - `LARECOMP_NO_WRITE_TRACKING=1` desliga.
- **Cache of vertices in copy mode:** filled by CPU, only the pages written, by upload buffer (`vc_flush`).
- **Result:**
  - ~12,500 cache hits per frame, ~30 KB updated per frame and ~23 page fouls per frame.
  - UBO copies: ~15 MB per frame, equal to Turnip.
  - **Render thread: ~37 → ~18,5 ms.** Correct image.
- ** Also:** each submission slot has its own command pool, restarted whole with `vkResetCommandPool`. Before, the implicit reset in `vkBeginCommandBuffer` returned and relocated driver memory.
- **Why the fps hardly went up (~15–17):** the frame is serial. The game waits for the fence of the frame it has just sent; the render thread translates (~18 ms); only then does the GPU work (~17–26 ms). All added up goes from 33 ms and rounds to 3–4 vsyncs.
  - The sending in pieces (`LARECOMP_VK_CHUNK_US`), which in Turnip overlaps translation and GPU, here doubles the busy time of the GPU (~38 ms) and does not change the period, probably because the clock of the GPU falls at intervals.
  - Next candidates: the driver's own cost per draw (`vkUpdateDescriptorSets` and `vkAllocateDescriptorSets` ~18%, `vkCmdBeginRendering` ~9%, `vkCmdDrawIndexed` ~10%) and understand the clock drop with shipping in pieces.

## Update: Cost per Draw (uniform dynamic buffers)

- **Change:** the uniform buffers of the graphics programs became `UNIFORM_BUFFER_DYNAMIC` (Android, up to the limit of the device; first the buffers of the game, then the tables of the runtime).
  - The offset of each copy exits the decryptor set and goes to `vkCmdBindDescriptorSets`.
  - Before, each draw copied its UBOs to a new place and almost no set coincided with the previous one: a set allocated and written by stage by draw.
  - `LARECOMP_NO_DYN_UBO=1` desliga.
- **Result (A/B on the same build, plateau):**

| Driver | Time in `sets` every 2 s | Surrender per frame | GPU per frame |
|---|---|---|---|
| Turnip | 307 → 234 ms | no measurable change (with profiler on) | igual |
| Qualcomm | 198 → 105 ms | 21,4 → 19,1 ms | igual |

  Correct image on both.
- **New account in the statistics log**(`binds N pipe N sets`), Turnip, per table: ~5.100 draws, ~850 pipeline exchanges and ~4,050 decryptor binds set. Each object has its own constants, so offsets change with each draw.
- **The cost of GPU per draw (~2 μs) has not changed.**Timetamps per draw (`LARECOMP_GPUPROF_DRAWS`) give median ~8 μs and minimum ~4 μs, but serialize the GPU and do not separate the cost from the bind. It was open if it is worth separating the UBOs (which change each draw) from the textures in their own sets.
- **Experiment (reverted): UBOs in own sets.**
  - **What was done:**The constant buffers of the game were to sets 2 and 3, separated from the textures (sets 0 and 1), and the texture sets were only reconnected when they changed.
  - **Turnip, plateau:** texture set binds of ~4.050 → ~910 per frame, with UBOs reconnected to each draw. The GPU was equal: 22.8 ms against 22.1 ms without separation, within the noise.
  - **Conclusion:**The cost of GPU per draw is not in the decryptor bind sets. The next suspect is the actual draw work in the shadower (e.g. constant readings via `cbr` with `min` by access) or the fixed draw cost of Turnip.
- **From where comes the cost of GPU per draw (Turnip, G-buffer, Table 2400, `[gpuprof]`).** Temporary test keys, removed after:

| Condition | G-buffer |
|---|---|
| Normal | 8,4–8,8 ms, ~3.600 draws |
| `TU_DEBUG=sysmem` | 8,9 ms |
| `TU_DEBUG=gmem` | 22,9 ms |
| 1 pixel Scissor on all Draws | 10,4 ms |
| Only 3 vertices per draw | **5,7 ms** |
| No rebind of sets within the same layout | ~2.2 μs per draw, equal to normal (the game starts to draw ~2.650 objects, because the wrong image changes cooking) |

  - Turnip already renders G-buffer directly into memory; forcing tiles (GMEM) gets worse.
  - Almost nothing is pixel work. The 1 pixel scissor even gets worse, by switching scissors every draw.
  - The cost is divided into ~1.6 μs fixed per draw (~5.7 ms) plus ~3 ms of vertex work.
  - Set binds do not explain the fixed cost.
  - **Conclusion:**the fixed cost is from Turnip itself per draw (state, vertices search), and only falls with less draws (LOD/distance from the game) or with lighter vertex shaders.
- **G-buffer's Vertex Shaders (Turnip).**I have read the GLSL of the six programs with the most Draws (`LARECOMP_SAVE_MSL=1`).
  - The work is that of the game itself: transformations in FMA, normalizations and, in some, skinning by constant indexed and texture readings in the vertex shader.
  - There are no `gfind` tables or global memory in these shaders.
  - The only addition of the translator by reading is the `min(layer, textureSize(t, 0).z - 1)` in the array textures, redundant in Vulkan, which already makes this clamp when sampling.
  - **Test:** take out this clamp, with alternate A/B and average of 250 frames: GPU 23.0 / 23.5. / 23.1 ms. No effect; reversed.
  - **Conclusion:** on the translator's side there is nothing to dry by vertex weighing. What's left in the GPU is the work of the game plus the fixed cost per Turnip draw.
