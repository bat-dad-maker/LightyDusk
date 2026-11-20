# Complete Breath of the Wild Performance Mods Survey and its BOTWRecompAndroid Application

This document consolidates the technical analysis and the in-depth survey of all major changes in performance developed for *The Legend of Zelda: Breath of the Wild* over the years on emulation and modding platforms (**Wii U / Cemu**, **Nintendo Switch / Yuzu / Ryujinx**, **Skyline**, **EggNS** and **UltraCam / NX-Optimizer**), detailing their mathematical/architectural principles and **as each technique can be used directly in static recompilation (`BOTWRecompAndroid`)**.

---

> **Check on BOTWRecompAndroid (21/10/2026, POCO F7 Ultra, Turnip, plateau scene).**This study combines external sources and inferences. Below, what was checked in the project. The rest of the text is worth as an idea map, not as a measurement.
>
> - **Confirmado:**
>   - The fall to 20 fps by `sead` (section 1.2), already corrected with `rt_frame_ticks`.
>   - The cost of GPU is by draw and not by pixel.
>   - O SMAA e o FSR 1 do runtime existem (abas Anti-aliasing e Upscaler).
>   - `LARECOMP_SKIP_FSHASH` existe.
> - ** It's not supported:**
>   - "40 to 47 fps in the Sanctuary" and "8.4 fps / 14,573 copies" do not appear in any measurement of the project. Without patch, BOTW 1.0.0 only runs at 30 or 20 (`sead` never chooses less than 2 vsyncs).
>   - G-buffer does not use "6 RGBA16F targets": are RG8, RGBA8 sRGB, RGBA8, R32F and R11G11B10, with albedo and normals connected in two slots each.
>   - Minor resolution does not reduce GPU time here: `LARECOMP_HANDHELD=1` (720p) kept ~23 ms. It helps in temperature and battery, not in fps.
>   - The game's FXAA costs **~0.6 ms** (luma 0.19 + FXAA/upscale 0.40 ms, `[gpuprof]`) and is **also the upscale from 1600x900 to 1080p**. You can't just skip it; trading it for a simple upscale would save ~0.3 ms, not 1.5–2.5 ms.
>   - The `.pchtxt` and RRS codes in section 2.B have no source. The `5287F008` annotation is wrong: it loads 0x3F80, not 0x3F800000. They're all from 1.6.0.
> - **Render distance:** according to the nx-optimizer itself, adjusts the camera's far plane ("far camera plane") and was made primarily for TOTK. In BOTW 1.0.0 the address is unknown, and reducing the distant plane would cut the horizon, not the nearby draws.
>   - G-buffer's ~3,600 draws in this scene are spread over 144 programs, without instanting and with maximum ~270 draws per shader. There's no dominant category to cut.
>   - The cost of ~2 μs of GPU per draw points to the state exchange for draw (pipelines and decryptor sets) in our renderer.
> - ** Dynamic FPS / 60 fps:** requires dozens of specific patches (stamine, arrows, ragdoll, water, menus) to be rediscovered in the target version. It's not a one-time delta function. It's after 1.6.0, where there are public patches to study.

---

## 1. Grounds for BOTW Engine and Original Gargals

The *Breath of the Wild* engine was developed on Nintendo's proprietary framework **`sead`** using Nintendo's physics library **Havok** and the game subsystem **`ksys`** (*KingSystem*). It is designed exclusively for fixed hardware limitations of Wii U and Nintendo Switch (Tegra X1):

1. ** Frame-Based Simulation Logic (30 FPS Fixed):**
   - The simulation loop advances at discrete intervals based on the assumption that 1 frame equals exactly $\frac{1}{30}\text{s} \approx 33.33\text{ms}$.
   - The integrations of physics, projectile speeds, aerodynamic drag, stamina drainage/reload rates, animation transitions, and script events contain values scaled fixed to 30 Hz.
   - **Naïve unlock for 60 FPS:** If the game simply runs at 60 FPS without patches, all speeds double ($2\times$). If set to 60 static FPS and hardware reaches 45 FPS, the game runs in slow motion ($0.75\times$).

2. **A Armadilha dos 20 FPS (*20 FPS Lock / VSync Drop*):**
   - In Switch, the `sead::Framework` module measures the time spent in the previous frame (Hardware `ticks`).
   - If the frame takes longer than $\approx 1.85 \text{ a } 1.9$ vsyncs ($> 31\text{ ms}$), the engine changes the NVN presentation range (`nvnWindowSetPresentInterval`) from **2** (30 FPS on screen from 60 Hz) to **3** (20 FPS).
   - On mobile devices (Android with Adreno/Mali GPUs), when the game drops to 20 FPS, the Android kernel frequency governor notes that the hardware is working only $\approx 25\text{ms}$ every $50\text{ms}$. The governor reduces CPU and GPU clocks to save power, preventing the game from returning to 30 FPS (*death spiral* of throttling).

3. **Agressive Dynamic Resolution (*Dynamic Resolution Scaling – DRS)***
   - The engine adjusts the render viewport between 75% and 100% (or 80% to 100%) in real time according to load. In emulators and recompilation, these constant changes force dynamic changes of scissor, break-out of rendering passes in the Vulkan and unnecessary resizing blits.

---

## 2. Historical and Technical Survey of Mods by Platform

```
                                  Evolution of BOTW performance mods
                                  
   [ Wii U / Cemu ]                [ Switch / Yuzu / Ryujinx ]            [ Android & Moderno ]
      (2017-2019)                         (2019-2023)                          (2023-2026)
  +------------------+                +------------------+                 +------------------+
  |      FPS++       |                | Static 60 FPS    |                 | Skyline / EggNS  |
  |  (Xalphenos/     |===============>| (.pchtxt / IPS)  |                 | (GMEM 540p hacks,|
  |   Crementif/     |                +------------------+                 |  fence bypass)   |
  |   rajkosto)      |                         |                           +------------------+
  +------------------+                         v                                    |
          |                           +------------------+                          v
          |                           | Dynamic FPS      |                 +------------------+
          +==========================>| (somerando/      |================>| UltraCam /       |
                                      |  ChuckLiddell)   |                 | NX-Optimizer     |
                                      +------------------+                 | (MaxLastBreath)  |
                                                                           +------------------+
```

---

### A. Cemu / Wii U: FPS++ (Xalphenos, Crementif, epigramx, rajkosto)

**FPS++** was the first implementation of history to accurately uncouple the frame rate of the game's internal logic. The modification consists of Assembly PowerPC modules injected into codecaves:

#### 1. Game Speed Calculation (`patch_GameSpeed.asm`)
Intercepts the main motor time count function (`0x031FA97C` in the Wii U v208 executable).
- **Tick measurement:** Reads the difference of barrier clocks (`timerTickSpeed = 62156250 Hz`).
- **Mobile Media Circular Buffer:** Maintains a history of 4 to 32 frames to dampen instantaneous peaks (*frame packing spikes*).
- ==References====External links==* Official website* Official website* Official website* Official website* Official website

#### 2. Patches in Specific Mechanicals:
- **Flurry Rush (`_changeFlurryRush`):** Modifies the Link's forward distance logger by applying `averageFPS1` to maintain the range identical to 30 FPS.
- **Bullet Time / Arrow Time Drain (`0x02D5F760`):** Apply `averageFPS1Inv` to the rate of stamina consumption when the arc is pulled into the air, preventing the stamina from evaporating in 1 second to 60 FPS.
- **Balistic Arrows (`0x020E5510`, `0x020E3EB0`):** Replaces static constants of gravity and impulse by `averageFPS30Inv` and `averageFPS0.5` to preserve the trajectory and distance of the straight shot.
- **Stamina Regeneration (`0x02D90D88`):** Loads the dynamic variable `averageFPS30` in place of fixed float `30.0f`.
- **Ragdolls physics (`patch_Ragdolls.asm`):**In the original game, the `RagdollApplyForce` function applied forces multiplied by the frame delta. At high rates, a single blow threw enemies off the map. The patch locks the ragdoll timestep in the constant float `0.03333334f` ($1/30$).
- **Water Physics (`patch_WaterPhysics.asm`):** The quadratic scaling of the drag in the water made floating objects not slow down. The patch introduced a formula with linear damping factor (`0.45f`): $$\text{timestep}_{\text{final}}} = \left((\text{timestep}_{30} - 1.0) \times 0.45 + 1.0\right) \times \text{timescale}$
- **Koroks chase (`patch_KorokChasing.asm`):**Hold the speed of Korok's brightness on the ground so they don't walk twice the speed.
- **Menu and Map navigation (`0x03415C1C`, `0x02F62B3C`):** Apply `averageFPS1Inv` to the map cursor speed and GUI.

#### 3. Synchronization and Fences Optimization (`patch_FenceMethod.asm` and `patch_VSync.asm`):
- **SkipFence / PerformanceFence:** BOTW emits frequent speeds to check completion of GPU commands. The patch implemented the option to skip non-essential CPU feces, releasing thread time to the emulator.
- **SwapInterval Control:** Allows to force the interval swap of the video subsystem to 0 (without lock/unlock), 1 (60 FPS) or 2 (30 FPS).

---

### B. Nintendo Switch / Yuzu / Ryujinx

In Switch, the game runs natively in ARM64 (Aarch64) architecture. The mods evolved from simple patches in `.pchtxt` files to dynamic code modules.

#### 1. 60 FPS Static Patches (`.pchtxt` / Atmosphere Cheats)
For the version **v1.6.0** (TitleID `01007EF00011E000`, Build ID `8E9978D50BDD20B4`):
```text
[v1.6.0 [60 FPS]]
04000000 01614954 52800008   ; mov w8, #0 (Deactivate lock check)
04000000 01614D84 5287F008   ; mov w8, #0x3F800000 (Float 1.0f vsync step)
04000000 01614DB0 52800020   ; mov w0, #1 (PresentInterval Force = 1)
04000000 01614DA4 12000021   ; and w1, w1, #1 (Ignora clamp minimum of 2 vsyncs)
```
- **How it works:** Forces the `sead::Framework` loop to submit the frame every 1 vsync (60 Hz) and fixes the step multiplier to 1.0.
- **Problem:** When the rate falls below 60 FPS (e.g. 40 FPS), the game suffers the proportional slow motion effect.

#### 2. Dynamic FPS (BOTW-DFPS / TOTK-DFPS per somerando and ChuckLiddell)
Created to remedy the limitation of the 60 static FPS:
- Replace the `sead::Framework` tick reading instruction with a call that consults the high-resolution `CNTVCT_EL0` register of the ARM64 processor.
- Converts the cycle count in real run time.
- Modifies the actor subsystem temporal advancement calls (`ksys::act`) to dynamically multiply the translation and animation vector by the fraction $\Delta t / (\frac{1}{30})$.
- **Result:** The game can swing freely between 20, 35, 45 or 60 FPS without accelerating or slowing down physics.

#### 3. Disable Dynamic Resolution (DRS Off) & Render Resolution Scaling (RRS)
- **Disable DRS:** Locks dynamic scaling routine in 1.0f, preventing the engine from reducing viewport in dense areas.
- **RRS Cheats:** Modify the renderer target table in the executable:
  ```text
  [Handheld RRS 75% (540p)]
  580F0000 02CC4100
  780F0000 000004C0
  680F0000 000002D0 00000500   ; 720, 1280 (Height 0x2D0, Width 0x500)
  580F0000 02CCEAE8
  780F0000 00001098
  640F0000 00000000 FFFFFFFF
  780F0000 00000118
  680F0000 3F400000 3F400000   ; Float 0.75f, 0.75f
  ```
  It allows you to select fixed internal resolutions: **360p (50%)**, **480p (67%)**, **540p (75%)**, **720p (80%) docked**, **900p (100%) docked** and **1080p (120% docked**.

---

### C. Emuladores Android (Skyline, Strato, EggNS)

In Android emulators running on Snapdragon processors (Adreno 6xx, 7xx, 8xx):

1. **Adjustment to GPU Tile Memory (GMEM):**
   - The Adreno GPUs use *Tile-Based Deferred Rendering (TBDR)* architecture with an ultra-fast memory in the die called **GMEM** (4 to 12 MB).
   - The original BOTW G-Buffer (1600x900 with 6 attachments RGBA16F/R16F) exceeds the limit of GMEM in native resolution, forcing *sysmem bypass* (reading and continuous writing in LPDDR5X RAM).
   - The use of patches of **540p or 720p** on mobile devices allowed the tiles to fit entirely in GMEM, eliminating memory bandwidth strangulation and reducing thermal heating.
2. ** Useless Fences Synchronization Elimination:**
   - BOTW records time reports in command buffers (`CMD_REPORT_COUNTER`). On Android, the cost of synchronizing commands with the timeline semaphore or CPU feces caused severe bottlenecks in rendering thread.

---

### D. UltraCam / NX-Optimizer (MaxLastBreath)

**UltraCam** represents the most sophisticated and modern optimization package of BOTW and TOTK for Switch and emulators, injected via `subsdk3` (ELF module with ExLaunch) and configured by `maxlastbreath.ini`:

| Recurso do UltraCam | Mecanismo Interno | Performance Benefit |
|---|---|---|
| **MaxDynamic Framerate (20–240 FPS)** | Replaces the `sead` delta-time accumulator with continuous interpolation and engine decoupling. | Allows arbitrary frame rates without breaking physics. |
| ** Separated MenuFPS (30 or 60 FPS)** | Dedicated lock only when GUI screens (`UI::Menu`) are active. | Avoids fast menus or uncontrollable hands at high rates. |
| **Quality Improvements** | Disable at instruction level: Engine FXAA, internal FSR filter and Dynamic Resolution. | Eliminates heavy post-processing post-processing passes from entire screen. |
| **Render Distance Scaling** | Modifies the global maximum range constant (`1000` Very Low, `5000` Low, `12500` Medium, `25000` Default, `35000` High). | Reduces the g-buffer draw calls count from ~3.250 to ~1.800–2,000 (**gain up to 25% to 30% FPS**). |
| ** Customizable FOV** | Adjusts the camera view field constant (standard `50.0`) by recalculating the projection matrix without stretching the 16:9 interface. | It allows for wider or narrower vision as preferred. |

---

## 3. Current BOTWRecompAndroid Diagnosis in Target Hardware

In tests performed at **Xiaomi POCO F7 Ultra (Snapdragon 8 Elite / Adreno 830v1)**:

- **Evolution of Performance at the Shrine of Resurrection:**
  - *Before optimizations:* **8.4 FPS**, 14,573 feedback copies and 8,767 passes every 2s.
  - * With `LDS` / `STS` support, UBOs directed invalidation (`guest_copy_invalidate`) and redundant snapshots elimination:* **40 to 47 FPS stable** (average frame time of $\approx 25\text{ms}$, reduction of $57\times$ in copies per frame).
- **GPU Current Gargal:**
  - A GPU trabalha $\approx 23\text{ ms}$ na cena 3D interna.
  - The profile shows that the cost **is not filler of pixels (fillrate)**, but **number of passes and amount of draw calls**:
    - O G-buffer de 6 alvos executa sozinho **$\approx 3.250$ draw calls** ($\approx 7\text{ ms}$).
    - There are about 220 to 230 small passes** additional with complete barriers.
- **Comportamento do Pacer:**
  - In [`tools/patches.py`](file:///d:/Projetos/Jogos/Android/BOTWRecompAndroid/tools/patches.py#L38-L55), we have already replaced the reading of ticks in `0x7000c4f4d4` (`sead` `f_7000c4f430`) with `rt_frame_ticks` (in [`runtime/nvn/objects.c`](file:///d:/Projetos/Jogos/Android/BOTWRecompAndroid/runtime/nvn/objects.c#L1102-L1120)), locking the maximum duration in 1.85 vsyncs to prevent the fall to range 3 (20 FPS).

---

## 4. Direct Application Guide on BOTWRecompAndroid

Unlike emulators that rely on memory injection in running time via JIT, **BOTWRecompAndroid recompiles the AArch64 code for C in advance (`botwgame.so`)**. This gives us absolute control to apply any of these corrections directly to pure C.

### 1. Implementation of Dynamic FPS Native in C
Instead of being limited to fixed locks of 30 FPS or 60 FPS static (where 45 FPS would cause slowness):
- In the lifter ([`tools/patches.py`](file:///d:/Projetos/Jogos/Android/BOTWRecompAndroid/tools/patches.py)), we can replace delta-time calculation with a real-time function in [`runtime/nvn/objects.c`](file:///d:/Projetos/Jogos/Android/BOTWRecompAndroid/runtime/nvn/objects.c):
  ```c
  // Example of C integration in runtime
  float rt_dynamic_timestep(float nominal_dt) {
      static uint64_t last_ns;
      uint64_t now_ns = SDL_GetTicksNS();
      if (!last_ns) { last_ns = now_ns; return nominal_dt; }
      float delta_s = (float)((now_ns - last_ns) / 1e9);
      last_ns = now_ns;
      
      // Safety limits: prevents physics burst in locking (minimum 20 FPS, maximum 120 FPS)
      if (delta_s > 0.050f) delta_s = 0.050f;
      if (delta_s < 0.008f) delta_s = 0.008f;
      return delta_s;
  }
  ```
- **Benefit:** The game starts responding with perfect $1.0\times$ speed at any rate (35, 42, 47 or 60 FPS), completely eliminating the feeling of drag or acceleration.

### 2. Draw Distance Reduction / LOD Constant (Inspired on UltraCam)
- The BOTW carries the maximum distance of view of actors and terrain objects from scalar constants (the `25000.0f` value documented on UltraCam).
- Apply a patch via `tools/patches.py` reducing this constant to `12500.0f` or `15000.0f`:
  - Decreases the amount of G-buffer design calls by up to **30% to 40%** (from $\approx $3,250 to $\approx 2,000 Draws).
  - Reduces GPU time from $\approx 23\text{ms}$ to **$\approx 15\text{ms}$**.
  - **Immediate result:** Allows hardware to reach **60 constant FPS** in Sanctuary and Overworld.

### 3. Post-Processing Pass Pruning and FXAA
- The BOTW runs full screen passes for FXAA and depth blur.
- In the Vulkan ([`runtime/nvn/vulkan/exec.c`](file:///d:/Projetos/Jogos/Android/BOTWRecompAndroid/runtime/nvn/vulkan/exec.c)) executable, we already have the `LARECOMP_SKIP_FSHASH` mechanism.
- By identifying the hash of the fragment shader responsible for the engine FXAA, we can skip the draw:
  - Eliminates entire screen reading and writing in the GPU.
  - Saves **1.5 to 2.5 ms** rendering time per frame.
  - Anti-aliasing can be delegated much more efficiently to the native SMAA or FSR of the runtime itself at the time of the present.

### 4. Inner Static Resolution and Selector in Menu (RRS)
- Currently, BOTW draws fixed at 1600x900 docked (or 1280x720 handheld via `LARECOMP_HANDHELD=1`).
- We can display an option in the Android settings (`Settings -> Internal resolution`):
  - **720p (Native Handheld):** Reduces G-buffer rendering area by 36% compared to 900p.
  - **540p (Performance Mode):** Allows you to fit the G-buffer perfectly into the Adreno GMEM, maximizing battery efficiency and keeping temperature low.
  - The FSR 1 scheduler already present in the `exec.c` runtime makes the buffer upscale clean of 540p/2020p for the native resolution of the mobile screen (3200x1440).

---

## 5. Summary Matrix and Optimization Priorities

| Mod / Optimization | Origem | Technical Impact | Prioridade no BOTWRecompAndroid |
|---|---|---|---|
| **Dynamic FPS Nativo** | FPS++ / BOTW-DFPS | It eliminates slow motion when the FPS is between 35 and 55 FPS; it lets you unlock 60 FPS with continuous speed 1.0x. | ** High** (fluid gaming experience) |
| **LOD / Render Distance Scaler** | UltraCam | Reduces the number of G-buffer draw calls by 30–40%, relieving the biggest current GPU bottleneck (23 ms $\to$15 ms). | **High** (Direct path to 60 FPS) |
| **Bypass de FXAA / Passes Redundantes** | Yuzu / UltraCam | Save 2 ms of GPU suppressing full screen post-processing passes that are redundant with the FSR/SMAA of runtime. | ** Average** (Clean optimization) |
| **Resolution Control (RRS 540p / 720p)** | Skyline / Cheats RRS | Reduces band consumption and fits passes into Adreno GMEM, reducing heat heating in long sessions. | ** Medium** (Battery and thermal stability) |
| ** Specific Physical Corrections** (Ragdolls / Water / Arrows) | FPS++ | Needed only when enabling Dynamic FPS above 30 FPS to prevent distortions in collisions and water physics. | **Dependant**(After activating Dynamic FPS) |

---
*Document compiled and integrated into the project repository in `docs/BOTW-PERFORMANCE-MODS.md`.*
