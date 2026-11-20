# Rotate direct game code to CPU (study, 2026-10-27)

Today the ARM64 code of BOTW is **recompiled**: `tools/gen.py` and `lifter.py` translate each function to C, and the cell compiles this C with zig (`botwgame.so`, ~120,000 functions, 10-15 min, up to 8 GB of memory). Switch and mobile are ARM64; the original code could run directly, as do MacNativeGames and totk-mobile on Apple. This document says what that would change here, what you take advantage of, the risks and a prototype with the criterion of stopping.

## Why look at it now

- **The limit of the 60 fps is the CPU of the game** (docs/BOTW-PORT.md, "What the thread you present waits"): in 1.9.0 the drawing thread of the game waits ~16 ms per frame for the main thread, which runs ~12-15 ms of logic of the game in a thread only. The GPU (~14 ms) and our render thread (~12 ms) are not the limit: taking 0.5 ms from the GPU did not change the period, nor ADPF tips, nor attach threads to the prime cores, nor take the relay from the core 0.
- Code recompiled for C and compiled at -O1 runs slower than the original code (compiled by Nintendo to -O2 or -O3): emulated flags, registers in a structure, SIMD per helpers, each indirect deviation per table. The slower it is not measured; the order of expected magnitude is 1.5-3x.
- Cellular compilation is the largest source of issues (#25, #47, #51, #52, #53: memory, ~80%, space). Running the original code, it ceases to exist: the configuration only copies the files.

## What others do

| Projeto | License | Status | How to rotate the code |
| --- | --- | --- | --- |
| [MacNativeGames](https://github.com/ly05010419/MacNativeGames) (ly05010419) | MIT | TOTK and BOTW playable on Mac; iPhone/iPad | Direct ARM64 code, rewritten module before loading |
| [totk-mobile](https://github.com/OJZen/totk-mobile) (OJZen) | Apache-2.0; core, MIT | TOTK 1.4.3 playable on Mac, iPad under 60 | Idem; the core comes from [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) (`core/tools/patch_nso.py`) |
| [MacNativeGames-Android](https://github.com/deivid22srk/MacNativeGames-Android) (deivid22srk) | MIT | Day scaffolding (2026-10-21): app, Vulkan, tests; ♪ The game never ran ♪ | You didn't get to the rewriter. |

The totk-mobile rewriter (`patch_nso.py`) changes in the module only what cannot run outside the Switch:

- `x18` (platform logger) read or written → a slot by thread;
- `mrs/msr tpidr_el0` (user's TLS) → a slot per thread;
- `mrs tpidrro_el0` (Horizon ThreadLocalRegion) → a slot per thread;
- `svc #n` and `mrs cntpct_el0` → a "door" to the host (a deviation to a stub that saves the registers and calls C);
- patches registered by build (the original bytes checked before switching), such as our `tools/patches.py`.

The MacNativeGames-Android has no code that fits, but its analysis (`docs/android-port-analysis.md`) is a good checklist for Android: anonymous executable memory (`mmap` + `mprotect`, allowed to `untrusted_app`, as in JITs), 16 KB pages, guest below 2^39, aliasing with `memfd`.

## What's good for you here?

The runtime was made for the recompiled code, but almost everything around it is independent:

- **Memory:** the guest already lives at his addresses (window 0x100000000-0x420000000, `main` at 0x100000000) and the recompiled code reads direct pointers (`LD64(a)` is `*(u64 *)a`). The original code would see the same memory.
- **Kernel and services:** `kernel.c` (threads, synchronization, core 0), IPC services, FS and romfs, audio, HID, DLC and saves receive `svc` and calls with a `Cpu *` - an assembly port mounts this struct with the registers and calls the same C.
- **NVN, Vulkan, shaders, pacifying, widescreen, upscalers, Android app, launcher:** nothing changes; the NVN entries that the game takes by `nvnBootstrapLoader` would become thunks in assembly for the same `NVN(...)`.

## What changes

| Parte | Today (recompiled) | Rodando direto | Effort |
| --- | --- | --- | --- |
| Game code | `botwgame.so` compilado no celular | The original `main` / `sdk` / `subsdk`, rewritten in the load and executable mapped at their address | Large: the rescriptor (Python on PC today; on mobile phone, C or Python that the app already takes) |
| `svc`, `x18`, TLS, contador | Viram C no lifter | Rewritten for ports and slots such as d2r-mobile | Medium |
| Chamadas host → guest | `guest_call` by function table | A stub that loads the `Cpu` registers and jumps to the address | Small |
| `tools/patches.py` (4-6 per version) | C in place of instruction | Trampolines in binary (deviation to a stub that makes the patch and back) | Medium |
| `setjmp`/`longjmp` (`hle/setjmp.c`) | HLE, WHY C DOESN'T COME BACK IN THE MIDDLE OF A FUNCTION | Unnecessary: the original works | Some |
| SIMD, flags, monitores exclusivos, FPCR | Emulados | Exact of the CPU itself | Some (and correct bugs that only emulation has) |
| Restart to the Home screen | `rt_unwind_point` at the input of each function | There are no points in the code: stop threads in `svc` and by the way | Medium, at risk |
| Core relay 0 | `svc` and `FN_ENTER` | Only in `svc` (that's where the Horizon changes thread too) | Small |
| `fnprof`, `[slow]`, crash report | `f_<address>` names | Direct guest addresses; sample profiles (`simpleperf` on guest PC itself) are simpler | Small |

## Riscos

1. ** `x18` and ShadowCallStack.** On Android `x18` is reserved; ShadowCallStack libraries use it as a pointer. Any `x18` in the game needs to be rewritten, and a signal (the writing tracking of pages uses SIGSEGV) that arrives with the playing game cannot depend on it. The rewriting of d2r-mobile covers this.
2. ** `tpidr_el0`.** The bionic keeps his TLS there; the game cannot write on it. Rewritten for slot; stubs restore the host when entering C.
3. ** `cntpct_el0`.** The kernel may not let the user space read the physical counter; the virtual (`cntvct_el0`) yes, and Snapdragon's is also 19.2 MHz, the frequency of Switch. Rewritten as d2r-mobile, or for `cntvct_el0`.
4. ** Executable memory.** Anonymous mapping with `PROT_EXEC` after writing: allowed for apps (it is the path of JITs), but each manufacturer can have its own policy. It's the first thing to prove in the POCO and Mali.
5. ** 16 KB pages** (some Android 15+): NSO segments are aligned to 4 KB; the loader needs to tolerate this (map the entire region and copy).
6. **What page writing tracking (`mprotect` + SIGSEGV) does with native code:** Today the handler knows that writing came from the recompiled code; with the original, the default PC is in the game code - the handler needs to continue working with `x18` /TLS of the active guest.
7. ** Work size.** It is to change the heart of the runtime. The recompiled path must continue to exist (other devices, debugging, the version that already works) behind a key, at least until the native proves himself.

## Proposed prototype (with stopping criterion)

In a branch, behind `LARECOMP_NATIVE=1`, in 1.0.0 (the simplest version) and then in 1.9.0:

1. **P0 - run.**The app maps the original rewritten `sdk` (only `svc`, `x18`, TLS, counter) at a fixed address with `PROT_EXEC`, and calls `nn::init::Start` from a thread with the prepared slots. Success: the first `svc` arrives at `kernel.c`. Prove questions 1-5 of the risks in the POCO (Adreno) and, if possible, in Mali.
2. **P1 - to the title screen.** `main` and `subsdk` also; NVN entries as thunks; 1.0.0 `patches.py` as trampolines. Success: the title screen with sound and the menu.
3. **P2 - measure.**The same save from 1.9.0 on the Great Plateau, `LARECOMP_PROF_SVC_TID` and `[pace]`: the time of the main thread per frame and the fps against the recompiled, in the same heat. **Stop if the gain is small** (the main thread less than ~25% faster): the cost of maintaining two paths is not paid.
4. **P3 - complete.**Restart to the home screen, crash report, the other versions, DLC, and only then change the pattern (the compilation on the mobile turns the backup path).

## P0 result (branch `native-p0`, 2026-10-27, POCO F7 Ultra, 1.9.0)

`LARECOMP_NATIVE=1` (`runtime/native.c`, `runtime/native_gate.S`): the original `main` and `sdk`, rewritten on the load, run directly on the CPU. **The game reached the title screen and the world** (the Great Plateau save, the right image, without crash), without using the `botwgame.so`.

- Rewritten: 152 `svc` (stubs entering `native_svc_gate`), 129 `tpidrro_el0` / `tpidr_el0` readings/writings (stubs finding `Cpu` thread by `TPIDR_EL0` in a scan table), 43 `mrs cntpct_el0` (see `cntvct_el0`: Snapdragon counter is 19,2 MHz, the same as Switch). `x18` got the game (~38,000 uses): Linux preserves it, and the ports return the host while C runs.
- The functions of runtime (HLE, NVN) have become code in the false addresses of `HLE_BASE`; `guest_call` enters the `native_enter`. `kernel.c`, services, NVN and Vulkan have not changed.
- Found bugs: (1) nested calls (the runtime calling the game from inside a door) lost the preserved registers - the `native_exit` now returns them to the `Cpu`; (2) two threads in the same slot in the table - now with scan.
- **Performance: without gain.**Alternate rounds, whether or not saved, whether or not heat (~62-66 °C): native 51.5 and 52.2 fps, recompiled 55.1 and 55.4 fps (render ~11.8 and GPU ~14.3 ms in both). The main thread remains ~70% occupied (~13 ms per frame) out of svc waits, as in the recompiled: its time is not the game code itself. The most likely is the active waiting by job workers (`t80` rotates in `SleepThread(-1)` and `WaitForAddress` waiting for jobs, which also ran well recompiled) and the cost of ports (each `svc` saves ~800 bytes and passes through the relaying of core 0).
- By prototype criterion (the 25% faster main thread), **the native path is not paid as fps gain**. What he would give is another thing: no compilation on the phone (without 10-15 minutes, without the end for lack of memory of issues #25/#47/#51/#53), less memory and less space. This remains to be decided; `patches.py` would be missing as trampolines (60 fps, widescreen, corrections of each version), restart for the home screen and crash report.

## After P0: no compilation on mobile (2026-10-27)

- **Configuration** "Native game code (experimental)" (Video, Android; `native_code` no ini, disconnected by default; `LARECOMP_NATIVE=0/1` passes over: with it the boot does not load `botwgame.so` and the configuration of the game files skips the compilation (the launcher no longer asks for "The game code needs building").
- **`tools/patches.py` corrections **turned stubs (`stub_fix`, `native_patch_gate`): the handler in C works on `Cpu` and chooses how to follow - the original instruction (copied on stub), skip it, the other address of the correction, or `ret`. The 12 (1.0.0: 2, 1.6.0: 2, 1.8.2: 4, 1.9.0: 4) confer the original word before switching.
- **Restart** (`LARECOMP_REBOOT_AT=3600:game`): 45 threads stopped at 20 ms, the game disassembled and reassembled, native again to the title screen. Threads stop at `svc` and calls to runtime, as before.
- **Crash report:** a failure within the game code copies the signal loggers to the `Cpu` (the report shows the game's and the caller chain) and names the pc for the modules' symbols (`main+offset`).
- **Test:** 1.9.0 (title, world, restart) and 1.0.0 (title, world). A round of 1.0.0 fell after ~25 s in the world (`nvnBufferGetAddress` received from the game an object with 0x600000001 in place of the pointer); the same round again passed 2 min without falling: a race that the native time exposes. 1.6.0 and 1.8.2 were not tested.

## In play by user (2026-10-27, 1.9.0, Sync lag off)

- 58-60 fps are largely stable, with CPU ~1 GHz and 35% of use; more falls than recompiled.
- **Correction, same day:** the tips in the cloud and fog in a blow ** are not native**. The user saw them in the recompiled also, with and without Sync lag, at 30 and 60 fps, without rounding texture coordinates (`LARECOMP_NO_VS_SNAP=1`) and in published versions 0.2.1 and 0.2.0 (recompiled with versionCode 19 and 20 for the test): an old defect that had just not been noticed. The curtains are also the old bug. What follows is the account as it was done:
- **Native only defects** (in the recompiled, in the same session, none of this):
  - In a blow, the cloud of the Mountain of Death (and the mist of the horizon) stretches sideways at ends by a picture** (recording 19:30, ~1:11, next to the spark of the hit). Link blinks sometimes when he takes the first blow or takes the first blow.
  - The ground curtains ([botw-stretched-vertices]) often appear: in conversation with the old man just after the tower and far away on the Great Plateau. For this old bug the native mode is almost guaranteed playback.
- Discarded: Sync lag (off, continues), 64 KB path (off), FPCR (the game connects FZ, bit 24, in its threads - the recompiled also respects), a wait for fence not yet sent (never happened).
- Next step, if resumed: a debugging frame (F9) at the moment of the blow with and without the native, comparing the constant buffers of the cloud; for the curtains, the same capture in the conversation after the tower.

Credits, if something is taken advantage of: the rewriting and stubs of d2r-mobile (MIT, with his warning), the ideas of totk-mobile (Apache-2.0: the license notice and a NOTICE if there is code) and of MacNativeGames (MIT), in README.
