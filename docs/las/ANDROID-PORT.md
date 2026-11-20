# Size plan: LAS Recomp for Android

Initial target: **Snapdragon (Adreno 6xx / 7xx) with the Vulkan Turnip** (Mesa) driver, loaded by the app. Other SoCs (Mali, Xclipse, Adreno with the Qualcomm driver) stay after Phase 3.

## Point of departure

What already exists and helps:

- **The Switch is AArch64, and Android as well.** The code generated uses the path `--simd asm` (FP/SIMD of the game as native inline asm). It's the same way as macOS Apple Silicon, which has already been proven. macOS also reserves `x18`, such as Android (ShadowCallStack), so the generated code should no longer use it.
- **The Vulkan backend** (`runtime/nvn/vulkan/`) and Maxwell shader translator → GLSL → SPIR-V already exist. `docs/linux.md` cites Turnip among Linux supported drivers.
- ** The `runtime/platform/linux.c` layer** already supports aarch64. She's the base of `platform/android.c`.
- **SDL3 and Dear ImGui** have official Android support.

What blocks or is different on Android:

| # | Problem | Where | Gravidade |
|---|---|---|---|
| 1 | Fixed memory window `0x70_0000_0000` – `0x73_2000_0000` (guest = host). In a forked process of zygote, with 39-bit VA, this track is where ART and libraries are already mapped. If there's a collision, you can't re-exec. | `rt.h`, `main.c` | **Alta** |
| 2 | The code generated is compiled on the device (Python `gen.py` + Zig). On Android there is no Python, and SELinux blocks `exec()` of binaries in the app's data folder (targetSdk ≥ 29). | `gamebuild.c` | Alta |
| 3 | The seed generator is a helper in PyInstaller (oead, evfl, keystone) and does not run on Android. | `rando_tab.mm` | Average (MVP may be vanilla only) |
| 4 | `lasgame.so` solves runtime symbols in the executable (`-rdynamic`). On Android, runtime is `libmain.so`, loaded into a linker namespace and out of the global scope. | `Makefile`, `gamelib.c` | Average (resolve on link) |
| 5 | Differences from mobile Vulkan: `VK_EXT_external_memory_host`, BC textures, `shaderInt64`, `dualSrcBlend`, `MAILBOX` | `vulkan/device.c`, `exec.c`, `textures.c` | Medium, measuring |
| 6 | Bionic: without `execinfo.h` / `backtrace()` before API 33, `/proc/self/exe` points to `app_process64`, and the ART uses `SIGSEGV` (sigchain) | `platform/linux.c` | Baixa |
| 7 | Lifecycle: pause/resume, destroyed and recreated surface, background audio. None of this is dealt with today. | `main.c`, `vulkan/device.c`, `audio.c` | Medium |
| 8 | Touch input: Today there is only keyboard and control | `hle/input.c`, `ui/` | Medium |
| 9 | Files: folder selector turns SAF (`content://`, no POSIX path). `curl` update check does not exist. | `launcher.mm` | Baixa |

## Architecture decisions (proposals)

- **App:**Gradle project from the `android-project` of SDL3 (`SDLActivity`), runtime in `libmain.so` (NDK, clang), ABI only `arm64-v8a`, `minSdk 30`, landscape orientation. Link with `-Wl,-z,max-page-size=16384` (pages 16 KB, Android 15+). `PLAT_PAGE` is already 16K.
- **Driver:** [libadrenotools](https://github.com/bylaws/libadrenotools) loads a `libvulkan_freedreno.so` (Turnip) from the app folder, as Skyline, Yuzu and derivatives do. Fallback: system driver (Qualcomm). The user can change the version of Turnip, because this will be necessary.
- ** Game code (MVP):** generated and compiled ** on PC** (`make gen` with `--simd asm` + NDK clang → `lasgame.so`) and copied to the device (adb or import). Nothing from the game goes on APK. Generating on your own device stays for Phase 6.
- ** `lasgame.so`:** Linked with `DT_NEEDED libmain.so` (the Android linker resolves the symbols of runtime by the dependencies of the lib itself) and loaded with `dlopen` from the internal folder of the app. `dlopen` of date app is allowed, only `exec()` is blocked.
- **Randomizer (MVP):**import seed folders generated on desktop. Generating seeds on the device is set for Phase 7.
- **Platform:** `runtime/platform/android.c`, starting from `linux.c`. `PLATFORM=android` in Makefile for Gradle objects and CMake calling the same list of fonts (or a `Android.mk` /CMake itself, see Phase 1).
- ** Scope:** This fork is for Android only. The other platforms do not change behavior: every change in shared code is behind `__ANDROID__` or a flag that only Android build passes, and the desktop compiles exactly as before. This also reduces diff in shared files and facilitates bringing upstream updates (peiikai/LASRecomp).

## Dynamic memory window (Android only)

**State (2026-10-15): implemented.**
- Generator validations:
  - without the flag, the output is identical to the previous (716 files);
  - with `--dyn-base`, no remaining absolute address (only 6 bitfield `BFM` masks, which are correct);
  - the generated C compiles with the NDK and accesses `lasgame_gb` by `ADRP` + `LDR`, without GOT.
- Runtime: The base is chosen by the `plat_reserve_guest` of Android. What the game boot uses (displacement of tables in `dispatch_init`, base warning by `gamelib.c`) will only be tested in Phase 2.

On the desktop, the guest window remains fixed on `0x70_0000_0000`. On Android, the base is chosen in runtime.

The game is PIC: Switch loads executables with ASLR, and `loader.c` already applies the NSO relocations to any base. The one who fixes the addresses is the compiler, when recording them as constants in the generated C. The "guest = host" model continues: loads and stores do not change, only where the window begins.

- **Choose from base** (`platform/android.c`): read `/proc/self/maps`, find a `GUEST_HI - GUEST_LO` hole (12.5 GB) **below 2^39**and above 4 GB (the game was made for a 39-bit space; the ART uses the 4 GB bass), book with `MAP_FIXED_NOREPLACE` and burn to `g_guest_lo`. Align base to 4 GB, so ADRP offsets remain trivial.
- ** `gen.py --dyn-base` ** (only Android build passes). Without the flag, the exit is identical to today's. With her:
  - `ADR` / `ADRP` and literal loadings (`lifter.py`) emit `B + offset`, with `const uint64_t B = g_main_base;` (or `g_sdk_base`) once per function;
  - return address and `c->pc` on calls (`gen.py:345-350`, `437-441`) saw `B + offset`;
  - `switch` of jump table and `L_dispatch` (`gen.py:448-462`) start using `switch (alvo - B)` with offset cases;
  - `rando_word()`, `rt_udf`, `rt_svc` e `FN_ENTER` recebem `B + offset`;
  - the function table (`gen.py:619`) saves offsets, and `dispatch.c` sums up the base when mounting the search;
  - `patches.py`: Keys remain in the canonical address `0x70…`, and the generator converts to offset with `a - 0x7000000000` (without rewriting ~60 entries).
- **Runtime:** in `rt.h` and `kernel.h`, `#ifdef __ANDROID__` defines `GUEST_LO` as variable (`g_guest_lo`) and `MAIN_BASE`, `SDK_BASE`, `HLE_BASE`, `TLS_BASE`, `HLE_HEAP`, `ALIAS_BASE`, `HEAP_BASE` and `GUEST_HI` as `GUEST_LO + offset`. On the desktop they remain constant `#define`.
  - The two static initializers (`dispatch.c:93` `hle_next`, `kernel.c:114` `tls_next`) are started in runtime.
  - The ~15 loose literals (`LIVE_SAVE 0x7001CBB220`, `qol.c`, `chamber.c`, `warp_tab.mm`, `save_editor.mm`, `fnprof.c`, `cmdbuf.c`, `gamebuild.c`,...) have seen `MAIN_BASE + offset` or `GUEST_LO..GUEST_HI` checks. On the desktop this compiles for the same constant.
  - The global shader memory already uses a variable (`maxwell_gmem_base = HEAP_BASE` in `vulkan/exec.c:1204`). Shaders don't have an embedded address.
- **Verification:**
  - a script that fails if the C generated with `--dyn-base` contains some literal in the range `0x70_0000_0000` – `0x73_2000_0000`;
  - in the apparatus, a test mode that forces an alternative base (by environment or extra Intent variable) to exercise two different bases;
  - measure the cost of `B + offset` with `simpleperf` (must be small: one add per materialized address).

## Fases

### Phase 0: evidence of viability (before carrying anything)

Objective: To answer questions that can change the plan. A minimum APK with SDL3, no game code:

1. ** Memory window.** Log `/proc/self/maps` inside the app several times (ASLR) and the size of the VA (inferred from the highest address in maps). Rotate the base choice algorithm of the "Dynamic Memory Window" section and confirm that you always find a hole of 12.5 GB below 2^39. Test apparatus: POCO F7 Ultra (Snapdragon 8 Elite, Adreno 830).
2. **Turnip via adrenotools.** Load Turnip, create an instance Vulkan 1.3 and log relevant extensions and features: `textureCompressionBC`, `shaderInt64`, `dualSrcBlend`, `depthClamp`, `wideLines`, `fillModeNonSolid`, `VK_EXT_external_memory_host`, `VK_EXT_custom_border_color`, `VK_EXT_vertex_attribute_divisor`, the present modes and features 1.1/1.2/1.3 that `device.c` requires (`shaderInt8`, `scalarBlockLayout`, `storageBuffer8/16BitAccess`, `shaderDemoteToHelperInvocation`, `maintenance4`,...). Repeat with Qualcomm driver to compare.
3. **Optional but cheap:** run the built **Linux aarch64** already existing (`make ARCH=aarch64`) in a proot Ubuntu in Termux, with Termux:X11 and Turnip on the same mobile phone. This validates the generated code and the Vulkan backend in Adreno before writing any Android code (Linux aarch64 today is "not tested on real hardware").

Output: a table of capabilities per device and the decision on the memory window.

#### Results (2026-10-15, POCO F7 Ultra: SM8750, Adreno 830, Android 16, kernel 6.6, pages of 4 KB, 16 GB)

The probe is in `android/probe/` (report on `/sdcard/Android/data/com.lasrecomp.probe/files/report-latest.txt`).

**Memory.** The results below were repeated in 5 executions.
- 39-bit VA. Free spaces of 12.5 GB or more between 4 GB and 2^39 are three: `0x1_0000_0000` – `0x62_8dcb_8000` (390 GB), `0x62_8dcc_5000` – `0x75_0000_0000` (74 GB) and `0x79_36f1_7000` – `0x7f_c560_5000` (26 GB).
- The dynamic base falls in `0x1_0000_0000`, about 3 to 5 ms, and reading and writing at the beginning, in the heap and in the end work.
- The fixed desktop window was also free. But the layout comes from the zygote and it only changes when the device restarts, so that doesn't guarantee anything. **Decision: dynamic base**, as planned.

**Vulkan.**

| | Qualcomm (sistema) | Turnip Gen8 V37 (adrenotools) |
|---|---|---|
| Driver | Vulkan 1.3.284, build 02/06/26 | Mesa 26.3.0-devel, Vulkan 1.4.363 |
| `vkCreateDevice` equals `device.c` | **falha**: falta `uniformAndStorageBuffer8BitAccess` | **Failure**: same reason |
| With the optional 8-bit features | OK | OK |
| `VK_EXT_external_memory_host` | no | no |
| `VK_EXT_device_fault` | sim | no |
| BC1–BC7, ASTC | nativos | nativos |
| `shaderInt64`, `dualSrcBlend`, `depthClamp`, `wideLines`, geometry/tessellation | sim | sim |
| `shaderFloat64` | no | no |
| Subgroup | 64, only fragment and compute | 64–128, all stages |
| 3-component vertex formats, `D24_UNORM_S8_UINT` | sim | sim |

What does that decide?
- **8 bits:** no shader uses 8 bits in uniform buffers (the only reference is `device.c` activation). On Android, `shaderInt8`, `uniformAndStorageBuffer8BitAccess` and `storageBuffer8BitAccess` are requested only when the driver has it.
- ** BC:** are native to both drivers. The BC decoder in the CPU exits the critical path; it is only for future GPUs without BC.
- **Subgroups:** `SHFL` in vertex shadow is already translated as local value, without subgroup (`translate.c`), so the limit of Qualcomm driver does not affect.
- **Global memory of the shaders:**Neither driver has host import. Alternatives: `adrenotools_import_user_mem` (only on the Qualcomm driver via KGSL) or the fallback by copy of Phase 3.
- **Not yet measured:**present mode and `preTransform`, because they need a surface. Stay for Phase One.

### Fase 1: build

- **Dependencies for Android arm64**(scripts with fixed versions and checksums, `tools/linux/build_deps.sh` style):
  - SDL3 (via `android-project`)
  - shaderc (sources come in NDK, `sources/third_party/shaderc`)
  - Vulkan Memory Allocator (headers only)
  - FFmpeg LGPL only with video codecs, as on desktop; alternative: implement `hle/movie.c` with NDK `AMediaCodec`
  - libadrenotools
- **`runtime/platform/android.c`:**
  - `plat_reserve_guest` with dynamic base (`android/common/guest_window.c`, validated in Phase 0), no re-exec
  - `plat_exec` and `plat_spawn` limited or absent
  - backtrace via `_Unwind_Backtrace`
  - crash handler installed in a way compatible with the ART sigchain
  - `plat_exe_path` → `nativeLibraryDir`
- **Logs:** `LOG` / `FATAL` → logcat (`__android_log_print`), or a redirection of `stderr` to logcat at the beginning.
- ** Paths** (`paths.c`): date root = `SDL_GetAndroidExternalStoragePath()` (`/sdcard/Android/data/<pkg>/files`, accessible by adb and file manager). `lasgame.so` is mandatory in the internal storage** (`SDL_GetAndroidInternalStoragePath()`) because the external is mounted `noexec`.
- **Gradle project** on `android/` (the CMake compiles `runtime/`, `third_party/imgui` and the Vulkan backend with `UIFLAGS=-x c++`, as on Linux) and the `make android-gamelib` target, which compiles `generated/` with NDK and links against `libmain.so`.

Ready when: APK installs, opens the ImGui start screen with Vulkan (Turnip) and log into logcat.

** State (2026-10-15): completed.** Validated in POCO F7 Ultra with Qualcomm driver and Turnip Gen8 V37 via adrenotools: full screen and landscape start screen, logcat logs (tag `LASRecomp`), guest window in `0x1_0000_0000`.
- Dependencies: `android/deps/build_deps.sh` (SDL3 3.4.16, shaderc 2026.3, Vulkan-Headers 1.4.350.1, VMA 3.3.0, volk).
- App: `android/lasrecomp/` (Gradle + CMake). The runtime turns `libmain.so`, loaded by `LASRecompActivity`. Custom drivers go in `Android/data/com.lasrecomp.android/files/drivers/` (the first `.so` or `.zip`).
- Runtime: all behind `__ANDROID__` (`platform/android.c`, `rt.h`, `vulkan/android_driver.c`, volk, `ANativeWindow` surface, optional 8-bit features, swapchain without pre-rotation, full screen always) or `LARECOMP_NO_FFMPEG`.
- Pendencies:
  - FFmpeg: `movie.c` is outside the build, and the background of the home screen is also (Phase 2);
  - swapchain pre-rotation (Phase 3);
  - seletor de pasta via SAF e toque (Fases 4 e 5);
  - review ~860 build notices.

### Phase 2: get to the first frame of the game

- Copy `extracted/` and `lasgame.so` by adb (temporary). Runtime copies `lasgame.so` from external storage to internal storage before `dlopen`.
- Enter the game directly (the equivalent of `--play` / `LARECOMP_NO_LAUNCHER`, extra Intent via or a flag file).
- Debug boot: kernel, threads, `nnMain`, services. Tools: `LARECOMP_FNPROF`, `rt_fatal_trace`, logcat and `simpleperf` for performance.

Ready when: the intro and title screen appear with sound.

** State (2026-10-15): the game kicks off.** In the Ultra POCO F7 with Turnip:
- modules in `0x1_0000_0000` (`--dyn-base`), kernel, threads and air services, save mounted;
- the opening videos play by FFmpeg (`android/deps/build_ffmpeg.sh`, via WSL), with the audio output open;
- ** About 12 fps**, with ~1.7 s of waiting of the GPU every 2 s and ~190 MB of uploading textures every 2 s (Phase 3).

Development flow (no setup on the device yet):
1. `android/build_gamelib.sh <exefs>` gera e compila a `lasgame.so`.
2. `android/push_game.sh <pasta com exefs/ e romfs/>` copies the files and generates `romfs.bin` on the device.
3. To get right into the game: `adb shell am start -n com.lasrecomp.android/com.lasrecomp.LASRecompActivity --es env "LARECOMP_NO_LAUNCHER=1"`.

The Switch system source is replaced by the DejaVu Sans that the app brings: Roboto does not have U+25A1, and the text code of the game does not treat null glyph.

The phase is not yet completed: confirm the sound on the device and pass the title screen (entry: physical control via SDL, or touch, Phase 4).

### Fase 3: Vulkan no Adreno/Turnip

Order defined by Phase 0 results:

- ** Optional 8-bit features** in `vkCreateDevice` (prerequisite, already validated in the probe).
- **Without `VK_EXT_external_memory_host` **(in both drivers): Today `exec.c` shuts off the global memory of the shaders (`LDG` / `STG` read zero). First measure **which game shaders use global memory; it may be little. Then a fallback per copy (a GPU buffer mirroring the used heap tracks, written back after compute), or `adrenotools_import_user_mem` on the Qualcomm driver.
- ** BC:** native to Adreno 830 (in both drivers). Decoding on CPU only if a GPU without BC appears.
- **Optional features:** in Adreno 830 only missing `shaderFloat64`. The translator should not generate `double`.
- **Present:** `MAILBOX` usually does not exist on Android. Use `FIFO` as default and respect `preTransform` (surface rotation).
- **Performance:**Internal lower standard resolution (the dynamic scaling code and FSR1 already exists),disk pipelines cache, `vkQueueSubmit` grouped and load/store ops care (GPU tilter: avoid unnecessary `LOAD` and `STORE` in attachments). Measure with Snapdragon Profiler.
- **General rules:**The desktop does not change behavior (Windows and Linux use the same backend). Every change in `vulkan/` is behind `__ANDROID__`, or conditioned to a capacity that the desktop always has, which gives the same result there.

Ready when: overworld, a house and a Dungeon render correctly at 30 or 60 fps in a Snapdragon 8 Gen 2/3.

**State (2026-10-15): The game runs at 30 fps stable in the open world**, and at 60 in light areas (POCO F7 Ultra, Turnip Gen8 V37, resolution 50%). What has changed since the ~12 fps of Phase 2:
- **Global memory:**With Turnip, the guest's heap is GPU memory mapped at its own address (`VK_EXT_map_memory_placed`, `vk_heap_place`), without copies. In the Qualcomm driver, copies by CPU (`gcopy`). Compute dyspatches read the heap through a decryptor window (adreno 2 GB limit) instead of copying tracks.
- **Shaders:** the `BRX` window only labels the switch itself (SSAO of ~20 ms for less than 1 ms). SSY/PBK/PCNT batteries are now solved in translation (`maxwell_static_stacks`): the open world GPU has dropped from ~18.5 to ~14 ms per frame.
- **CPU:**Runtime went from `-O0` to `-O2` (Gradle's build Debug had no optimization). Function dispatch without stack frame and with larger cache (from 13% to 5% of the game thread). Hash of the fragment shader and the memoized pipeline key. UI loop without active waiting.
- **Synchronization:**The fences of the game cover his work, and not present it, which awaits the composer. The `SyncWait` dropped from ~10 to ~1.4 ms per frame.
- **Android:**Cache of pipelines saved during the game, surface recreated when returning from the background, performance overlay, IME off with connected control.
- **Diagnosis:** `LARECOMP_GPUPROF_AT`, `LARECOMP_VK_PROF`, `LARECOMP_LOG_PACE`, `LARECOMP_LOG_SYNCSEQ`, `LARECOMP_LOG_COUNTER`, `LARECOMP_SHADER_STATS`, `LARECOMP_SHADER_SUBST` and `LARECOMP_SAVE_MSL`, in addition to simpleperf.

For 60 fps in the open world, we're missing:
- ** GPU:** ~14 ms per frame, while the game only chooses mode 60 below its limit. Next step: swap the Shader State Machine with deviations by `if` /Lassoes (the heavy vertex shaders run with 48 registers and only 4 waves).
- **CPU:**~18 ms per frame in the game thread in mode 30, being game code ~10 and recording ~8 Vulkan and driver. Option: Save commands to own thread.

Known problems: switching the resolution with the running game can close the app; without swapchain pre-rotation; heap above 2 GB against the decryptor limit outside the computer dyspatches.

### Phase 4: Real Android (life cycle and input)

- **Ciclo de vida:**
  - `SDL_EVENT_WILL_ENTER_BACKGROUND`: pause guest threads, pause audio, wait for idle GPU
  - `SDL_EVENT_DID_ENTER_FOREGROUND`: recreate the surface and swapchain (today they are created once in `device.c`)
  - Don't lose the save if the system kills the app in background.
- **Touch:** a virtual control (overlay ImGui or designed by the presenter) that feeds the same path as `hle/input.c` and `bindings.c` that physical control uses, with configurable layout and opacity. The buttons on the screen follow the button icons that already exist.
- ** Physical controls:** already work via SDL3. Check that the Back button opens the menu (the equivalent of F1) instead of closing the app.
- **ImGui:** scale by DPI, larger touch targets and virtual keyboard (`osk.mm` already exists).
- ** Screen:** immersive (without bars), notch/cutout, wide aspect (the project widescreen fits well on screens 20:9 mobile).

### Phase 5: User installation (without adb)

- **Choose game folder:** `ACTION_OPEN_DOCUMENT_TREE` (SAF) via JNI and copy to date root. The launcher already copies the files to `extracted/`; only the source changes (`content://`).
- Import `lasgame.so` generated on PC (MVP) and seed folders.
- Import fonts (`SDL_ShowOpenFileDialog` works on Android and returns URIs).
- Update check: swap `curl` with HTTP via JNI (`HttpURLConnection`), or hang up on Android.

### Phase 6: Generate the game code on the device itself

Only after the game goes well. Options:

| Option | Gerador | Compilador | Comment |
|---|---|---|---|
| A | Inbuilt Python (Chaquopy or CPython from NDK) + capstone, unicorn and lz4 compiled for Android | Zig packaged as `jniLibs/arm64-v8a/libzig.so` (`nativeLibraryDir` allows `exec`) | Less rewritten, large APK |
| B | Rewrite parts of `gen.py` that need to run on the C device (capstone and unicorn have API C) | Same as A | More work, more robust |
| C | Continuar gerando no PC | (nenhum) | Zero effort, worse for the user |

Consider the build time and the heating of the device: compile in background with progress, with `-O1` as today.

### Phase 7: Randomizer on device

The seed generator depends on oead and evfl (C++/pybind11) and keystone. The options mirror Phase 6: Python built in with these libs compiled for Android, or continue importing seeds generated on the desktop (the seed ID already plays a seed exactly the same).

### Phase 8: Distribution

- APK by GitHub Releases (sideload). The Play Store is unlikely by third party content policy.
- Nothing from the game at APK. Review the README license table (adrenotools is BSD-2, Turnip is MIT, and the GPL part enters only if Phase 6/7 packages unicorn, oead and keystone).
- `docs/android.md` page in the `docs/linux.md` style: tested devices, Turnip version, how to report bugs.

## Riscos principais

1. ** Dynamic base:** an absolute literal forgotten in the generator or runtime causes subtle bugs. The check script and the two-base test exist to catch this.
2. **Adreno 830 (8 Elite) on Turnip:**Turnip Gen8 loads and creates the device, but is still an experimental build (Mesa main with patches). Keep the Qualcomm driver as an alternative.
3. **Global shadow memory without host import (confirmed on both drivers):**can break computer effects until fallback exists.
4. **RAM:** the guest heat can reach 3.25 GB (`HEAP_MAX`), more textures and the process. Apparatus with 8 GB or more are the realistic target. Watch the low memory killer.
5. **Termic and performance:**The code generated with `-O1` is good on Apple M1; a Snapdragon 8 Gen 2+ should suffice, but intermediate devices may require low internal resolution.
6. **Turnip × Table version:** behaviour varies by version. Fix a tested version and let the user switch.

## Next steps

Made (2026-10-16): APK release with everything on the phone (Phases 5 and 6), published as v0.1.0 and v0.1.1 on GitHub: the app chooses the game folder by the Android selector (SAF), copies `exefs/` and `romfs/`, generates `romfs.bin` and compiles the game code on the device itself (`runtime/gamebuild.c`, ~20 min on Snapdragon 8 Elite); the Turnip driver is also chosen by the app. Touch buttons disappear whenever there is a connected control (item 1 from the previous list).

Then in this order:

1. **60 fps sustained (made, 2026-10-16, v0.2.0):** on the open map, 98% of the time at 60 and no frame above 18 ms (before ~55-70%). What has changed: a rendering thread translates the command lists while the game does the following table, and delivers to the GPU parts in the pass ends (`runtime/nvn/vulkan/async.c`; LARECOMP_VK_ASYNC=0 shuts down); the core 0 of the Emulated Switch (the threads of this core take turns, as in the console: the game's task system had a running condition that a faster main thread exposed - `runtime/kernel.c`; the pacer distinguishes the frames from 2 steps of the game itself from a 60 frame that delayed; ADPF targeting 60 fps in both threads; three synchronisation fixes in kernel (condvar, referee, timeout). Fixing the main thread in the fastest cores (LARECOMP_PIN) was turned off: it exposed the same running condition.
2. **Manufacturing (advertised):** the lsfg-vk changed license (MIT on v1, GPLv3 on v2, now CC BY-NC-ND 4.0, which prohibits derivative works) because of forks generated by AI; only the v1 continues MIT, and using it would go against the explicit request of the author. Decision (2026-10-16): first the 60 fps sustained; then, if it still makes sense, generation of frames itself with AMD FidelityFX FSR 3 (MIT), without Lossless.dll (the game does not give motion vectors: quality is doubt).
3. **In addition to Turnip's own driver (lighting made, 2026-10-16, v0.3.0):** on Qualcomm's driver the game now runs with lighting, at 30 fps. Without `VK_EXT_map_memory_placed`, the global memory of the shaders passes through an arena in the GPU (`exec.c`, gres_*): the regions that a shadower writes stay resident, the CPU writings are detected by comparison and copied by the GPU, the GPU results come back block by block; a margin of 4 KB before each region (the light counter is before the list); and the shaders record the accesses outside the tables, which have seen regions learned. The Qualcomm compiler locks with the structurizer: off on this driver. Missing: performance in this mode (~640 MB of copies every 2 s) and testing Mali/Xclipse (should use the same path).