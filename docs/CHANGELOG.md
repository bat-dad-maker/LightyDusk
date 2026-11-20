# Changelog

## 0.4.8-en.1 (BOTWRecompAndroid EN, 2026-11-20)

English-only fork of [mamcjr/BOTWRecompAndroid](https://github.com/mamcjr/BOTWRecompAndroid) 0.4.8-beta.

- **English everywhere:** the Portuguese documentation, notes, scripts, comments and user-facing strings were
  translated to English (machine-assisted, then reviewed). The Portuguese README was removed.
- **English-only menus:** the in-app menu translation tables (Portuguese, Spanish, French, German, Italian, Dutch,
  Russian, Japanese, Korean, Chinese) and the Android `values-*` resources were removed; Android's per-app language list
  offers English only. The game's own text-language option is unchanged and still selects the language of the game's
  files.
- **Installs beside the original:** application ID `com.botwrecompandroid.en`, app name "BOTWRecompAndroid EN",
  version 0.4.8-en.1. Its data lives in `Android/data/com.botwrecompandroid.en/`, so game files and saves from the
  original app are not shared automatically.
- **Smooth loading (experimental), off by default:** see below. **Untested on real hardware.**
- **CI:** a GitHub Actions workflow builds the release APK on every push to `main`.

### Smooth loading (experimental)

Settings > Video > "Smooth loading (experimental)". Off by default, and read when the app starts; when off, file
reads and save writes take exactly the upstream code paths. When on:

- after each read of the game data (`romfs.bin`), a background thread asks the kernel to read the next 4 MiB ahead
  into the page cache (`posix_fadvise` WILLNEED), with at most 16 requests waiting;
- a save file open for writing is kept in memory (up to 64 MiB) and the game writes to that copy; on Flush, Commit or
  close, a copy is written by a background thread to a temporary file, synced, and renamed over the save (atomic
  replace). Pending saves are written out when the app goes to the background or exits;
- the existing pipeline prewarming is turned on too.

Written and tested on a desktop only (a small test of the save path); **untested on real Android hardware**. If you see
save or loading problems, turn it off.

---

## Upstream release notes (English)

English versions of the upstream release notes, condensed: the lead paragraph, the section headings and the first
sentence of each top-level item. Releases that only had Portuguese notes were machine-translated. The full notes are
at each "Upstream" link.

## v0.4.8-beta (2026-11-20)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.4.8-beta

**Beta 40.** Fixes for game threads that could wait forever, a freeze report that shows where the game's main thread is, and texture swaps without waiting for the render thread on every GPU.

### The freeze after a few minutes (#401, #489)

- **Freeze report:** besides the thread list, the log now says who holds core 0, for how long, who is queued for it, and where the holding thread is at that moment.
- **Three fixes for ways a game thread could wait forever:**
- The report no longer calls an in-game restart a freeze.

### Performance

- **Texture slot swaps without waiting for the render thread on every GPU** (before: only on Mali and low RAM).
- **Mali (and copy mode): compute work chained without extra barriers**, and a barrier that could be lost is now made.

### Known issues

- **The game can freeze after a few minutes** (#401 on Mali-G1, #489 on an Adreno 710): the fixes above may help, and the new report should show the cause.
- **Mali-G720 / G615: the app can close after ~35 min** because memory mappings grow (#496).
- **4 GB RAM:** close to 20 fps in the world, and fps drops as the device heats up.
- **RADV on Xclipse 530:** the game crashes inside the RADV driver when entering the world (#383).
- **1.0.0 in combat:** enemies can leave black lines when they die.
- Rain falls slower than on the console.

## v0.4.7-beta (2026-11-18)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.4.7-beta

**Beta 39.** The map and minimap no longer go dark with RAM saving on, there's a new Video > Shadows option, and the game waits less for shaders. Several changes came from contributors (thank you, Propinbo and HallanBR!).

### Fixes

- **Dark map and minimap with "RAM saving" on (#471, #478):** zoomed in on the map, or on the minimap, the picture went dark.
- **Frame rate 40 / 45 on 120 Hz screens (#455):** HyperOS keeps a game at 60 Hz until it asks for more, and the mode fell to 30.

### New

- **Video > Shadows:** turns the game's shadows off.
- **Fewer waits for shaders:**
- **Mali: texture changes without stopping the game:** texture slot changes go into the render thread's queue on every Mali, without waiting for it to empty (Propinbo, #485).
- **Freeze report:** if no frame finishes for 5 s, the log now also lists each of the game's threads and what it is waiting for.
- **Performance:** the clock is read once a pass, not once a draw (HallanBR, #452), and command-list copies note their fences while copying (HallanBR, #456).

### Known issues

- **The game can freeze after 30-40 min** (#401 on Mali-G1, #489 on an Adreno 710): the GPU finishes everything and the game's threads stop.
- **4 GB RAM:** close to 20 fps in the world, and fps drops as the device heats up.
- **Mali:** memory can still grow over long sessions.
- **RADV on Xclipse 530:** the game crashes inside the RADV driver when entering the world (#383).
- **1.0.0 in combat:** enemies can leave black lines when they die.
- Rain falls slower than on the console.

## v0.4.6-beta (2026-11-17)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.4.6-beta

**Beta 38.** Link no longer sinks through river water after playing for a while, the game is back at full speed 1 s after every load, and several crashes are fixed. Several fixes came from contributors (thank you, Propinbo!).

### Fixes

- **Water without collision after a while (#296, #438):** after 30-60 min, Link sank into rivers and walked on the bottom, with the water still drawn above him.
- **Slow for ~10 s after every load (#465):** after loading the save or a fast travel, "Sync lag" (Video) stayed off for 300 frames.
- **Touch controls never shown (#477):** on some phones the fingerprint sensor registers as a "controller" (OnePlus 7 Pro), and the app hid the touch controls.
- **The game closed at a cutscene with a movie (#470):** when the dump's `Movie` folder was empty.
- **The game closed at start with a damaged DLC (#472):** an incomplete `aoc.bin` (an interrupted install, for example) made the game abort.
- **Native Mode, 1.0.0 and 1.6.0:** the game's own locks now have the right memory ordering (412 in 1.6.0, 252 in 1.0.0; 1.8.2 and 1.9.0 have none).
- **Mali: bilinear filtering where the GPU lacks it (#436):** on some Mali GPUs (Immortalis-G925), some texture formats can't be filtered, and "phantom" water showed on dry ground near rivers.

### New

- **Video > Pipeline prewarming (experimental, off):** records the pipelines used and prepares them while the save loads, for fewer hitches when entering the world.
- **Freeze diagnostics:** if no frame finishes for 5 s, the log records where each thread was waiting.
- **Benchmark:** stall diagnostics, a cooling wait and presentation timing (Propinbo, #315).

### Known issues

- **Dark map and minimap with "RAM saving" on** (#471, #478; Adreno 710 with TurnipZ, Exynos 2400 with RADV): turn RAM saving off.
- **Mali-G1 (Dimensity 9500):** the game can freeze in busy scenes (#401).
- **4 GB RAM:** close to 20 fps in the world, and fps drops as the device heats up.
- **Mali:** memory can still grow over long sessions.
- **RADV on Xclipse 530:** the game crashes inside the RADV driver when entering the world (#383).
- **1.0.0 in combat:** enemies can leave black lines when they die.
- Rain falls slower than on the console.

## v0.4.5-beta (2026-11-16)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.4.5-beta

**Beta 37.** The game's shaders no longer stay in memory forever, FXAA is lighter, and four improvements arrive from [TOTKDroid](https://github.com/mamcjr/TOTKDroid): Native Mode's clock on MediaTek phones, two more of the game's audio effects, and textures and vertices in copy mode (Mali, Xclipse).

### Memory

- **Shaders freed when the game restarts:** each time the game was restarted or sent back to the start screen from the app's menu, it left every shader and pipeline built so far in memory.
- **Unused shaders freed on Mali and other non-Adreno GPUs (#450):** shaders were never freed during play (1513 alive after 30 min, when a Mali-G720 closed at Android's memory limit).

### From TOTK

- **Native Mode on MediaTek (Dimensity):** these phones' time counter runs at 13 MHz, not the Switch's 19.2 MHz, so in Native Mode every span the game timed came out at 68% of its length.
- **Audio:** two of the game's effects that used to pass straight through are now processed: the auxiliary buffer (sound the game sends to an effect and gets back) and the 3D reverb.
- **Lighter FXAA:** the same FXAA 3.11 with its own defaults, at half precision.
- **Lighter FSR 1:** edges are analysed once, and where there is no edge it becomes a simple filter (in TOTK: 1.00 to 0.50 ms on the Adreno 830).
- **Textures in copy mode (Mali, Xclipse):** a texture the CPU wrote is now checked whole, not by samples, so no change slips through (in TOTK, new letters were missing from text).
- **Vertices in copy mode:** what was drawn this frame or the last no longer leaves the cache, instead of being copied whole again every frame when there was more data than the cache held.

### Known issues

- **4 GB RAM:** close to 20 fps in the world, but still below that on average, and fps drops as the device heats up.
- **Mali:** memory can still grow over long sessions.
- **Mali-G1 (Dimensity 9500):** the GPU can hang after a few minutes (#401).
- **RADV on Xclipse 530:** the game crashes inside the RADV driver itself when entering the world (#383).
- **Water without collision** after a while, in some places (#296).
- **1.0.0 in combat:** enemies can leave black lines when they die, and the game can close during a fight.
- Rain falls slower than on the console.
- Short grass in the distance only appears up close.
- On Qualcomm's stock driver, the pause menu's background is black.

## v0.4.4-beta (2026-11-15)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.4.4-beta

**Beta 36.** The game is back at full speed right after a menu, and frame generation reaches more Mali GPUs.

### Fixes

- **Fps drop after any menu (#374, #439, #433):** for about 12 s after closing the pause menu, the inventory or the rune selection, the game ran at 20-27 fps, then went back to normal.
- **Stuck at 20 fps after a hitch:** a single hitch (a save, a menu, shaders compiling) no longer takes the game to 20 fps.
- **Frame generation: "no Lossless.dll" with the file in the right place (#437, #385):** Android shows the file in Download/ to the app but doesn't let it open it without All files access.
- **Frame generation on GPUs without "null descriptors" (#402):** frame generation is no longer unavailable on drivers without that feature, such as the Mali-G615's (r44).

### Known issues

- **4 GB RAM:** close to 20 fps in the world, but still below that on average, and fps drops as the device heats up.
- **Mali:** memory still grows over long sessions, and the game can close after 30-40 min.
- **Mali-G1 (Dimensity 9500):** the GPU can hang after a few minutes (#401).
- **RADV on Xclipse 530:** the game crashes inside the RADV driver itself when entering the world (#383).
- **Water without collision** after a while, in some places (#296).
- **1.0.0 in combat:** enemies can leave black lines when they die, and the game can close during a fight.
- Rain falls slower than on the console.
- Short grass in the distance only appears up close.
- On Qualcomm's stock driver, the pause menu's background is black.

## v0.4.3-beta (2026-11-15)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.4.3-beta

**Beta 35.** The generation of frames (LSFG) now works on mobile phones in which Android left the game at 60 Hz even with a faster screen, as in ROG Phone 8.

### Fixes

- **Frameless frame generation on quick screens:**With the game at 40–50 fps and the screen at 60 Hz, there was no room left for any frame generated, and the game remained the same.

### Known issues

- **4 GB RAM:** close to 20 fps in the world, but still below that on average, and the fps drops when the device heats up.
- **RADV on Xclipse:** the game falls within the RADV driver itself when entering the world (#383, #413).
- **Mali-G1 (Dimensity 9500):**The GPU can lock after a few minutes (#401).
- **1.0.0 in combat:** Enemies can leave black lines when they die, and the game can close in the fight.
- **Mali:**The grass can still turn black.
- The rain falls slower than on the island.
- The grass down in the distance only comes up close.
- In the Qualcomm factory driver, the bottom of the pause menu is black.

## v0.4.2-beta (2026-11-15)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.4.2-beta

**Beta 34.** Turnip phones where the GPU was waiting on the CPU got faster: on a Galaxy Tab A9+, the game went from ~15 to ~19 fps in the world. Low-RAM mode can now be chosen in the menu.

### Performance

- **Turnip, every phone:** each submission of work to the GPU waited for the GPU to finish the previous one.
- **Output resolution "Auto" with RAM saving on:** 720p at most.
- **Tip for weak devices:** 50% internal resolution, with Snapdragon GSR and 720p output, runs as fast as 25% with FSR and output at screen resolution, and looks much better.

### New

- **Video > RAM saving:** Automatic, On or Off.

### Known issues

- **4 GB RAM:** close to 20 fps in the world, but still below that on average, and fps drops as the device heats up.
- **RADV on Xclipse:** the game crashes inside the RADV driver itself when entering the world (#383, #413).
- **Mali-G1 (Dimensity 9500):** the GPU can hang after a few minutes (#401).
- **1.0.0 in combat:** enemies can leave black lines when they die, and the game can close during a fight.
- **Mali:** grass can still be black.
- Rain falls slower than on the console.
- Short grass in the distance only appears up close.
- On Qualcomm's stock driver, the pause menu's background is black.

## v0.4.1-beta (2026-11-14)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.4.1-beta

**Beta 33.** Low-RAM mode is much faster on 4 GB devices: on a Galaxy Tab A9+, the game went from ~5 to 11–15 fps. This release also adds vibration, a 30 fps thermal cap, and pausing the game in the background.

### 4 GB RAM devices

- **Faster:** in the world, from ~5 fps in 0.4.0 to **11–15 fps** on the Plateau.
- **No low-memory kills** in the test runs.
- **Missing ground fixed:** half-size textures, from 0.3.3, now apply only to compressed formats.
- **Flickering letters in menus fixed:** while a menu is open, the app no longer splits the frame into several submissions.
- **Tests on the Tab A9+:** several runs from the same save copy, from the title screen to the world and the inventory.

### New

- **Vibration:** the game's vibration now reaches a controller's motors when Android exposes them.
- **Thermal cap:** while Android says the phone is too hot, the 60, 40 and 45 fps modes hold at 30 fps.
- **Background pause:** with the app minimized (Home, recent apps), the game stops instead of running unseen.
- **Start screen banner:** notices above the version, which can change without a new app version.

### Fixes

- **Screens narrower than 16:9 (4:3, 16:10) with unlocked aspect:** the image was squeezed from the sides (as on an Anbernic RG477M).
- **RADV on Xclipse** (#234, #383, #387): with "game memory through a shared file" on, the game closed (SIGBUS) right at the start.
- **Wrong texture up close:** the LOD a shader queries could come out wrong, and the game sampled the wrong mip.
- **Mali and Xclipse, copy mode:** fixed a case where a draw went outside its render pass (from TOTKDroid).
- **Log:** the first line names the version, the device and Android.

### Known issues

- **4 GB RAM:** still under 20 fps in the world, and fps drops as the device heats up.
- **Not tested:** minimizing the app right while the game is saving.
- **1.0.0 in combat:** enemies can leave black lines when they die, and the game can close during a fight.
- **Mali:** grass can still be black.
- Rain falls slower than on the console.
- Short grass in the distance only appears up close.
- On Qualcomm's stock driver, the pause menu's background is black.

## v0.4.0-beta (2026-11-13)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.4.0-beta

**Beta 32.** This release adds optional frame generation (LSFG) and a new performance overlay. It also fixes the game getting stuck at 20 fps after a menu and lowers memory use on Xclipse and other non-Adreno GPUs.

### New

- **Frame generation (LSFG), optional:** Lossless Scaling's frame generation, using **your own** `Lossless.dll` from Lossless Scaling for Windows (not included).
- **New overlay:**

### Fixes

- **FPS stuck lower after a menu, the runes or a save** (#339, #374): a hitch could leave the game's pacing in its 20 fps mode for good.
- **Xclipse (Samsung) and other non-Adreno GPUs:** the 1,500-pipeline limit that already applied to Mali now applies to every GPU except Adreno (#327, #345).
- **Mali and others:** the program and pipeline caches now grow with use instead of having a fixed size.
- **Map cut off in unlocked aspect** (#349): since 0.3.2 the map showed in 16:9 with bars and cut off, like the scope.
- **Message box in the middle of the screen in unlocked aspect** (#304): signs and messages lost the rounded ends of their box.
- **↑/↓ arrows with the camera rune** (#202): in the touch controls, for the zoom.
- **Controller gyro with swapped axes** (#338): to try another axis order without a new APK, in Settings > Driver compatibility > Driver variables: `LARECOMP_PAD_GYRO=z,y,x`.
- **Minimap at the top on 1.6.0** (#273): the option now applies only from 1.8.2, where it works.
- **Mods with uncompressed files** (#353): a mod that ships `X.bfres` instead of `X.sbfres` is now read.
- **Game file search** (#373): the screen shows how many folders it has checked, so it doesn't look stuck.
- **More useful logs:**

### Known issues

- **1.0.0 in combat:** enemies can leave black lines when they die, and the game can close during a fight.
- **Mali:** grass can still be black.
- Rain falls slower than on the console.
- Short grass in the distance only appears up close.
- On Qualcomm's stock driver, the pause menu's background is black.

## v0.3.4-beta (2026-11-09)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.3.4-beta

### BOTWRecompAndroid 0.3.4-beta (beta 31)
A small compatibility hotfix for 6 GB devices. Version 0.3.3 automatically enabled the memory-saving profile on devices reporting less than 6 GiB of RAM, including phones sold with 6 GB. Some users reported errors that did not occur before. The automatic profile is now limited to devices with up to 4 GB; 6 GB devices return to the normal texture, buffer, and cache settings. The manual `LARECOMP_LOW_RAM` override remains available, and Mali's separate pipeline limit is unchanged.

## v0.3.3-beta (2026-11-08)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.3.3-beta

**Beta 30.** Focused on Mali and low-RAM phones: fewer crashes after long sessions, the black grass fixed, and less memory used below 6 GB.

### Fixes

- **Crashes and slowdowns on Mali after ~30 minutes** (#310, #316): the logs showed that the app kept every graphics pipeline it had ever made, and Mali's driver holds memory for each one (5,500 pipelines and 2.3 GB at the crash).
- **Black, stretched grass on Mali** (#307, #258): the grass reads a texture in a format the GPU can't filter linearly.
- **Crash at "Open your eyes" in Native mode** (#305): a call to an empty function now returns zero, as Recompiled mode already did.

### Phones with less than 6 GB of RAM

- Mipmapped textures start at their second level (half the size, a quarter of the memory).
- Smaller GPU memory blocks and a buffer cache that scales with the RAM.
- The thread that prepares the frames runs on the fastest cores.

### Known issues

- **1.0.0 in combat:** enemies can leave black lines when they die, and the game can close during a fight.
- Rain falls slower than on the console.
- Short grass in the distance only appears up close.
- On Qualcomm's stock driver, the pause menu's background is black.
- Native mode: Link sometimes flickers on the first hit he takes.

## v0.3.2-beta (2026-11-08)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.3.2-beta

**Beta 29.** Fewer crashes with the Slate's camera, the unlocked-aspect HUD without jumps or flicker, an option to keep the minimap at the bottom, and the red shadows fixed on the Adreno 840.

### New

- **Minimap at the top** (Controls; #254): with it off, the minimap stays in its usual place even with the touch controls.
- **Built-in benchmark** (PR #278, Propinbo): measures fixed scenes from a copy of the save (adb only, for testing; see the PR).
- **Foldables** (PR #284, ilovz73ai-sketch): on a foldable's inner screen (Galaxy Z Fold, Pixel Fold), phone motion follows the right rotation.

### Fixes

- **The Slate's camera closing the game when taking photos** (#300, Recompiled mode): after reading an empty pointer, the game called a function that doesn't exist.
- **Red shadows on the Adreno 840** (#32): with the Turnip Gen8 drivers, shadows on the ground turned red.
- **HUD after a pause** (#261, #259): leaving a menu, the HUD sat in the middle of the screen for a second before going out to the edges.
- **Scope in unlocked aspect** (#262): the scope's frame is 16:9, and the world showed at its sides.
- **Inventory in unlocked aspect** (#195): the black box flickering at the right edge and the corners flashing as the pages turned are gone.
- **Aim when turning the phone round**: turning the phone from one landscape to the other no longer moves the aim.
- **Validation errors** (PR #255, Propinbo): points without a set size and empty copies no longer cause validation errors.

### For Mali and Xclipse

- The log now says why **game memory through a shared file** wasn't used when the driver refuses the buffer (#264, #277).
- The memory mappings log now also shows the memory in use, the pipelines and the textures.

### Known issues

- **1.0.0 in combat:** enemies can leave black lines when they die, and the game can close during a fight.
- Rain falls slower than on the console.
- Short grass in the distance only appears up close.
- On Qualcomm's stock driver, the pause menu's background is black.
- Native mode: Link sometimes flickers on the first hit he takes.

## v0.3.1-beta (2026-11-04)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.3.1-beta

**Beta 28.** Fewer crashes with runes and the Slate's camera, gyro aiming like the Switch's screen, and shared memory trying another way on Xclipse.

### New

- **Phone motion like the Switch's screen** (Menu > Controls; #211, #247): with it on, tilting and turning the phone work as on the Switch in handheld mode.
- **Game memory through a shared file (experimental):**

### Fixes

- **Crashes with Magnesis, Stasis and the Slate's camera** (#235, #241, #248, #247): one of the game's checks on the rune's or camera's target, before that target was ready, read an empty pointer.
- **Weather forecast** (#243): with the minimap up top (touch controls), the forecast floated in the middle of the screen.
- **Memory mappings on Mali** (#249): the log now shows where the count comes from (the game's memory, the GPU driver, the rest), to find what still reaches Android's limit in long sessions.

### Known issues

- **1.0.0 in combat:** enemies can leave black lines when they die, and the game can close during a fight.
- Rain falls slower than on the console.
- Short grass in the distance only appears up close.
- On Qualcomm's stock driver, the pause menu's background is black.
- Native mode: Link sometimes flickers on the first hit he takes.

## v0.3.0-beta (2026-11-04)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.3.0-beta

**Beta 27.** No more crashes after half an hour on Mali GPUs, the fast travel's dissolve back, 40 fps on 120 Hz screens and even 30 fps on 60 Hz screens.

### New

- **40 / 45 fps** (Video > Frame rate; #207, #216): between 30 and 60, at the rate your screen shows evenly - 40 fps on a 120 Hz screen, 45 on a 90 Hz one (on a 60 Hz one it stays at 30).
- **Touch controls:**
- **Driver compatibility** (start screen > Settings):

### Fixes

- **Mali: crashes and freezes after 30+ minutes** (#196, #176, #178, #223, #201): the app's write tracking split the game's memory into smaller and smaller pieces until Android's limit on memory mappings.
- **Fast travel** (#220; PR #210, by Propinbo): Link turned flat blue and vanished at once.
- **Even 30 fps on 60 Hz screens** (PR #183, by Propinbo): some frames showed for 50 and 16.7 ms instead of 33, a judder the fps counter didn't show.
- **Slow phones at 30 fps** (#89): since 0.2.9 Android could lower the clocks until frames went past 33 ms, and the game dropped to ~20 fps at any quality.
- **Unlocked aspect:**
- **Sound silent after the background** (PR #96, by 3122380113): the SDL patch is really in the APK now.

### Known issues

- **1.0.0 in combat:** enemies can leave black lines when they die, and the game can close during a fight.
- Rain falls slower than on the console.
- Short grass in the distance only appears up close.
- The water's reflection follows the camera when Link walks sideways.
- On Qualcomm's stock driver, the pause menu's background is black.
- Native mode: Link sometimes flickers on the first hit he takes.
- At 120 Hz, PR #183's fine 30 fps pacing is off for now (it made things worse there).

## v0.2.13-beta (2026-11-03)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.2.13-beta

**Beta 26.** An option of 20 fps, more gestures in touch controls and the end of the leaves that appeared on the floor.

### New

- **20 fps** (Video > Frame rate: Auto, 20, 30 or 60): for weaker device.
- **Controles de toque:**

### Fixes

- **leaves on the ground** (#168): dark leaf spots appeared on any type of floor around the Link and disappeared seconds later.
- **Tap conversations** (#200): with touch controls, the response options went up with the minimap and were split.

### Known issues

- **1.0.0 in combat:** Enemies can leave black lines and risks when they die, and the game can close during the fight.
- Free aspect: in the menu page exchange, the page appears cut at the edge of part 16:9 while sliding.
- The grass down in the distance only comes up close.
- The reflection in the water accompanies the camera when Link walks sideways.
- In the Qualcomm factory driver, the bottom of the pause menu is black.
- Native mode: Link sometimes blinks when taking the first blow.

## v0.2.12-beta (2026-11-02)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.2.12-beta

**Beta 25.** Touch Controls Overhaul: touch controls with icons, gestures and the minimap at the top. And a fix for horse names.

### Touch Controls Overhaul

- **Action layout:** while you play in the world, buttons show what they do instead of A, B, ZR.
- **The game's own icons,** read from your game files (nothing is shipped in the APK):
- **Gestures:**
- **Rune fan:** a tap on power uses the rune.
- **Shield surfing:** double-tap the shield button and hold on the second tap.
- **Minimap at the top:** while the touch controls are on screen, the minimap, time, temperature and sensor move to the top right corner, out of the buttons' way.
- **Map from the minimap:** tap the minimap to open the map.
- **Layout editing:**

### Fixes

- **The map in unlocked aspect:** it now fills the screen, with its frame at the screen's edges.
- **Horse names** (#165): the stable asks for the name in UTF-16, and the app sent UTF-8.

### Diagnostics

- **Mali closing after ~30 min** (#176): Mali's driver crashed as a draw pass began.

### Known issues

- **1.0.0 in combat:** enemies can leave black lines and streaks when they die, and the game can close during a fight.
- Free aspect: when the menu turns its page, the page shows cut at the 16:9 part's edge while it slides.
- Short grass in the distance only appears up close.
- The water's reflection follows the camera when Link walks sideways.
- On Qualcomm's stock driver, the pause menu's background is black.
- Native mode: Link sometimes flickers on the first hit he takes.

## v0.2.11-beta (2026-11-02)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.2.11-beta

**Beta 24.** Fixes for motion aiming with a controller connected, and for mods that wouldn't turn off.

### Fixes

- **Motion aiming did nothing with a controller connected, or stopped after the right stick was used** (#149, #175, #78):
- **A mod imported "in place" wouldn't turn off** (#167, #190):

### Known issues

- **1.0.0 in combat:** enemies can leave black lines and streaks when they die, and the game can close during a fight.
- Free aspect: when the menu turns its page, the page shows cut at the 16:9 part's edge while it slides.
- Short grass in the distance only appears up close.
- The water's reflection follows the camera when Link walks sideways.
- On Qualcomm's stock driver, the pause menu's background is black.
- Native mode: Link sometimes flickers on the first hit he takes.

## v0.2.10-beta (2026-11-01)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.2.10-beta

**Beta 23.** A fix for the "curtains": terrain stretched up to the sky after a fast travel or a tower.

### Fix

- **Terrain stretched into "curtains" after fast travel or activating a tower:** the oldest bug on the known issues list.

### Known issues

- **1.0.0 in combat:** enemies can leave black lines and streaks when they die, and the game can close during a fight.
- Free aspect: when the menu turns its page, the page shows cut at the 16:9 part's edge while it slides.
- Short grass in the distance only appears up close.
- The water's reflection follows the camera when Link walks sideways.
- On Qualcomm's stock driver, the pause menu's background is black.
- Native mode: Link sometimes flickers on the first hit he takes.

## v0.2.9-beta (2026-11-01)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.2.9-beta

**Beta 22.** Half the GPU memory on Mali: the game's textures no longer take 8 times their size.

### Fix

- **RAM filling up, "low memory" kills and the frame rate dropping to 1-5 after a few minutes on Mali ([#143](https://github.com/mamcjr/BOTWRecompAndroid/issues/143), [#155](https://github.com/mamcjr/BOTWRecompAndroid/issues/155), [#162](https://github.com/mamcjr/BOTWRecompAndroid/issues/162), [#152](https://github.com/mamcjr/BOTWRecompAndroid/issues/152), [#153](https://github.com/mamcjr/BOTWRecompAndroid/issues/153), [#154](https://github.com/mamcjr/BOTWRecompAndroid/issues/154), [#89](https://github.com/mamcjr/BOTWRecompAndroid/issues/89)):**

### Also in this version

- **Frame rate 30 (fixed):** the app gives Android the time budget of 30 fps instead of 60.

### Known issues

- **1.0.0 in combat:** enemies can leave black lines and streaks when they die, and the game can close during a fight.
- Free aspect: when the menu turns its page, the page shows cut at the 16:9 part's edge while it slides.
- Short grass in the distance only appears up close.
- Sometimes, right after a tower's activation scene, the terrain shows stretched into "curtains".
- The water's reflection follows the camera when Link walks sideways.
- On Qualcomm's stock driver, the pause menu's background is black.
- Native mode: Link sometimes flickers on the first hit he takes.

## v0.2.8-beta (2026-10-31)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.2.8-beta

**Beta 21.** A fix for the frame rate that fell over a session on Mali GPUs.

### Fix

- **Frame rate falling with playtime, until a restart ([#89](https://github.com/mamcjr/BOTWRecompAndroid/issues/89)):** on GPUs without the heap import (Mali, and stock drivers that lack it), the app keeps a 48 MB area on the GPU for the global memory shaders use.

### Known issues

- **1.0.0 in combat:** enemies can leave black lines and streaks when they die, and the game can close during a fight.
- Free aspect: when the menu turns its page, the page shows cut at the 16:9 part's edge while it slides.
- Short grass in the distance only appears up close.
- Sometimes, right after a tower's activation scene, the terrain shows stretched into "curtains".
- The water's reflection follows the camera when Link walks sideways.
- On Qualcomm's stock driver, the pause menu's background is black.
- Native mode: Link sometimes flickers on the first hit he takes.

## v0.2.7-beta (2026-10-31)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.2.7-beta

**Beta 20.** Gyroscope fixes with controllers, a warning for DLC mods, and GPU work for Mali and Adreno.

### Fixes

- **Gyroscope with a controller ([#78](https://github.com/mamcjr/BOTWRecompAndroid/issues/78), [#137](https://github.com/mamcjr/BOTWRecompAndroid/issues/137), [#115](https://github.com/mamcjr/BOTWRecompAndroid/issues/115)):**
- **Relics of the Past without the DLC ([#65](https://github.com/mamcjr/BOTWRecompAndroid/issues/65)):** the mod closes the game at the title screen when the DLC isn't installed.
- **Overlay behind the menu ([#142](https://github.com/mamcjr/BOTWRecompAndroid/issues/142)):** the performance overlay is hidden while the menu is open, like the touch controls; it used to show through the panel.
- **Large textures kept up to date:** a texture larger than the frame's checking budget (4 MB) is also checked whole now and then.

### GPU

- **Mali:** render targets are made without a mutable format (only their sRGB pair), which lets the driver compress them (AFBC) and saves bandwidth.
- **Passes that start by clearing their targets:** the clear becomes the pass's own load.
- **Shaders (from TOTKDroid):**
- **CPU:** the game threads' yield no longer calls the system's, which took ~30 µs each.

### Known issues

- **1.0.0 in combat:** enemies can leave black lines and streaks when they die, and the game can close during a fight.
- Free aspect: when the menu turns its page, the page shows cut at the 16:9 part's edge while it slides.
- Short grass in the distance only appears up close.
- Sometimes, right after a tower's activation scene, the terrain shows stretched into "curtains".
- The water's reflection follows the camera when Link walks sideways.
- On Qualcomm's stock driver, the pause menu's background is black.
- Native mode: Link sometimes flickers on the first hit he takes.

## v0.2.6-beta (2026-10-31)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.2.6-beta

**Beta 19.** A motion sensor choice, Latin American Spanish and Canadian French, and fixes for Mali and the Adreno 750.

### New

- **Motion sensor ([#115](https://github.com/mamcjr/BOTWRecompAndroid/issues/115)):** Controls now has **Motion sensor: Auto, Phone or Controller**.
- **Spanish (Latin America) and French (Canada) ([#128](https://github.com/mamcjr/BOTWRecompAndroid/issues/128)):** the game has both texts in every version, but the app only offered Spain's and France's.

### Fixes

- **Striped terrain on the Adreno 750 ([#129](https://github.com/mamcjr/BOTWRecompAndroid/issues/129)):** in 0.2.5 the in-place reads of draw targets (made for the Adreno 8xx only) turned on for every Turnip, and on the Adreno 7xx the terrain and the start screen's background were striped.
- **Mali: missing text and the app killed for memory ([#121](https://github.com/mamcjr/BOTWRecompAndroid/issues/121), [#118](https://github.com/mamcjr/BOTWRecompAndroid/issues/118)):** on newer Mali drivers (r54p1 and later), importing the 3 GB heap into the GPU left the game with no text and grey blocks on the title (Mali-G1-Ultra), or got the app killed by Android (Mali-G610).
- **Mali: FPS collapsing after a load, and a crash leaving a shrine ([#89](https://github.com/mamcjr/BOTWRecompAndroid/issues/89), [#131](https://github.com/mamcjr/BOTWRecompAndroid/issues/131)):** after a teleport or leaving a shrine, the memory ranges the app learns filled every draw's table, and a draw that didn't fit had the CPU copy everything: up to 15 GB of uploads every 2 s and 6 FPS until the game restarted.
- **Import fonts on Android ([#122](https://github.com/mamcjr/BOTWRecompAndroid/issues/122)):** every font picked was "not recognized", because Android hands the file over as `content://`.
- **Faster frames on the CPU:** the game's threads that record draw commands fought over one lock on every command; the lookup now takes none.

### Known issues

- **1.0.0 in combat:** enemies can leave black lines and streaks when they die, and the game can close during a fight.
- Free aspect: when the menu turns its page, the page shows cut at the 16:9 part's edge while it slides.
- Short grass in the distance only appears up close.
- Sometimes, right after a tower's activation scene, the terrain shows stretched into "curtains".
- The water's reflection follows the camera when Link walks sideways.
- On Qualcomm's stock driver, the pause menu's background is black.
- Native mode: Link sometimes flickers on the first hit he takes.

## v0.2.5-beta (2026-10-30)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.2.5-beta

**Beta 18.** The game files as one `romfs.bin`, 1.0.0's videos back, and a batch of crash fixes.

### New

- **One romfs.bin instead of ~41,000 files:** copying the whole romfs/ folder to the phone is slow and fails on many devices.

### Fixes

- **1.0.0's videos frozen:** the tower video (and the others) stopped at the first frame.
- **Crash at a cutscene with a video ([#113](https://github.com/mamcjr/BOTWRecompAndroid/issues/113)):** when a video couldn't be opened (Zora's Domain, with a dump used in place), the game closed a moment later.
- **Black distant trees with dynamic resolution ([#112](https://github.com/mamcjr/BOTWRecompAndroid/issues/112)):** away from 1x scale, the lighting pass skipped some trees, which stayed black with no fog.
- **Crash coming back from the background ([#108](https://github.com/mamcjr/BOTWRecompAndroid/issues/108)):** sometimes Android gives the new window the old one's address, and the game tried to draw on the dead surface (`vkCreateSwapchainKHR ...
- **The DLC's romfs taken for the game's ([#87](https://github.com/mamcjr/BOTWRecompAndroid/issues/87)):** with the game and the DLC in the same folder, the setup could pick the DLC's romfs and fail.
- **Controller gyroscope ([#78](https://github.com/mamcjr/BOTWRecompAndroid/issues/78), [#115](https://github.com/mamcjr/BOTWRecompAndroid/issues/115)):** 0.2.4's rule (a quarter second of fast movement) didn't let a slow aim, or a controller at rest, take over.
- **Crashes and hitches with the GPU's global memory ([#54](https://github.com/mamcjr/BOTWRecompAndroid/issues/54), [#89](https://github.com/mamcjr/BOTWRecompAndroid/issues/89)):** when the table of memory ranges filled up, it merged everything into one huge range: uploads grew a lot, and a late write-back corrupted the game's memory.
- **Restart and exit:** restarting the game clears leftover pending writes, and exiting no longer fails on a destroyed mutex.
- **Renamed Adreno 7xx ([#103](https://github.com/mamcjr/BOTWRecompAndroid/pull/103)):** on Turnip, the GPU heap's "Automatic" turns on again on these devices.
- **Menus:** the Copy them / Use them in place choice and 11 more lines that had no translation, now in all 11 languages.

### Known issues

- **1.0.0 in combat:** enemies can leave black lines and streaks when they die, and the game can close during a fight.
- Free aspect: when the menu turns its page, the page shows cut at the 16:9 part's edge while it slides.
- Short grass in the distance only appears up close.
- Sometimes, right after a tower's activation scene, the terrain shows stretched into "curtains".
- The water's reflection follows the camera when Link walks sideways.
- On Qualcomm's stock driver, the pause menu's background is black.
- Native mode: Link sometimes flickers on the first hit he takes.

## v0.2.4-beta (2026-10-29)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.2.4-beta

**Beta 17.** Mali performance back, keeping 0.2.3's fixes, and the game code build that always failed on some phones.

### Fixes

- **Mali slow in 0.2.3 ([#88](https://github.com/mamcjr/BOTWRecompAndroid/issues/88), [#89](https://github.com/mamcjr/BOTWRecompAndroid/issues/89), [#93](https://github.com/mamcjr/BOTWRecompAndroid/issues/93), [#54](https://github.com/mamcjr/BOTWRecompAndroid/issues/54), [#61](https://github.com/mamcjr/BOTWRecompAndroid/issues/61), [#94](https://github.com/mamcjr/BOTWRecompAndroid/issues/94)):** to end the crashes and garbled text, 0.2.3 put the memory the CPU and GPU share in Mali's **uncached** memory type.
- **Build failing after a second ([#52](https://github.com/mamcjr/BOTWRecompAndroid/issues/52), [#92](https://github.com/mamcjr/BOTWRecompAndroid/issues/92)):** when the build resumed where it had stopped, it read the previous run's log and asked the compiler for a file that didn't exist yet (`file_open FileNotFound`).
- **Gyroscope with a USB-C controller ([#78](https://github.com/mamcjr/BOTWRecompAndroid/issues/78)):** with the GameSir X5 Lite, the gyroscope worked for the first aim and then stopped.

### Known issues

- Free aspect: when the menu turns its page, the page shows cut at the 16:9 part's edge while it slides.
- Short grass in the distance only appears up close.
- Sometimes, right after a tower's activation scene, the terrain shows stretched into "curtains".
- The water's reflection follows the camera when Link walks sideways.
- On Qualcomm's stock driver, the pause menu's background is black.
- Native mode: Link sometimes flickers on the first hit he takes.

## v0.2.3-beta (2026-10-29)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.2.3-beta

**Beta 16.** Game files and mods used straight from your folder, with no 16 GB copy, mods that add files (Relics of the Past), and many graphics fixes.

### What's new

#### Game files without a copy

- **How to use it:** on Game Files, the new **Game files** option has *Copy them* (as before) or *Use them in place*.
- **Permission:** using them in place asks for Android's all-files access, so the app can read your folder while the game runs.
- **Careful:** with the files in place, don't move or delete the folder you picked; the game needs it every time.
- It works in native mode and recompiled (the built code still goes to the internal folder).

#### Mods: used in place, and mods that add files

- **Mods that add files and folders:** before, a mod could only replace files the game already has.
- **DLC mods:** a mod folder named after the DLC's ID (`01007EF00011F001`) now goes into the DLC's data, not over the game's.
- **Mods in place:** with *Use them in place* on, importing a mod doesn't copy anything either (not tested yet: please report problems).

### Graphics fixes

- **Dark dots and lines on grassy ground ([#21](https://github.com/mamcjr/BOTWRecompAndroid/issues/21), [#80](https://github.com/mamcjr/BOTWRecompAndroid/issues/80)):** rows of dark dots, straight across the screen, over the grass, more visible above 50% resolution.
- **Spikes in the smoke on a hit:** Death Mountain's smoke, the horizon's fog and defeated monsters' smoke turned into stars of spikes for a few frames on every hit.
- **"ME" instead of "GAME OVER":** the Game Over screen sometimes showed only part of its title.
- **Free aspect (Video > Aspect ratio > Unlocked):**

### Other fixes

- **White water with Sync lag ([#43](https://github.com/mamcjr/BOTWRecompAndroid/issues/43)):** starting the game with Sync lag on, the water turned white.
- **Crashes on Mali ([#71](https://github.com/mamcjr/BOTWRecompAndroid/issues/71), [#54](https://github.com/mamcjr/BOTWRecompAndroid/issues/54), [#62](https://github.com/mamcjr/BOTWRecompAndroid/issues/62), [#67](https://github.com/mamcjr/BOTWRecompAndroid/issues/67), [#74](https://github.com/mamcjr/BOTWRecompAndroid/issues/74)):** the memory the game shares with the GPU used a type that, on Mali, isn't kept in sync between the CPU and the GPU: crashes in a different place each time, doubled text, odd colours.
- **Crash at 0x34000003c ([#70](https://github.com/mamcjr/BOTWRecompAndroid/issues/70), [#72](https://github.com/mamcjr/BOTWRecompAndroid/issues/72), [#75](https://github.com/mamcjr/BOTWRecompAndroid/issues/75)):** on phones where the game's memory isn't on the GPU, the app makes pages read-only to see what the CPU writes, and sometimes didn't recognise one of those faults as its own.
- **Native mode asking for a build:** a setup made in native mode sometimes still opened the build bar.
- **Saves another app put in ([#77](https://github.com/mamcjr/BOTWRecompAndroid/issues/77)):** saves copied into the app's folder by a file manager with root, or by a save editor, kept the wrong owner, and the game closed with `svcBreak`.
- **Game memory on the GPU on 6 GB phones ([#83](https://github.com/mamcjr/BOTWRecompAndroid/issues/83)):** that option reserves about 3 GB at once.
- **Gyroscope with a USB-C controller ([#78](https://github.com/mamcjr/BOTWRecompAndroid/issues/78)):** a controller that reports a gyroscope but sends no motion (GameSir X5 Lite) turned the phone's off.

### Known issues

- Free aspect: when the menu turns its page, the page shows cut at the 16:9 part's edge while it slides.
- Short grass in the distance only appears up close.
- Sometimes, right after a tower's activation scene, the terrain shows stretched into "curtains".
- The water's reflection follows the camera when Link walks sideways.
- On Qualcomm's stock driver, the pause menu's background is black.
- Native mode: Link sometimes flickers on the first hit he takes.

## v0.2.2-beta (2026-10-28)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.2.2-beta

**Beta 15.** Introducing **native mode (experimental)**: the game's code runs directly on the phone's CPU, with nothing to build.

### What's new

#### Native mode (experimental)

- **No build:** in native mode, the setup only copies the game files.
- **How to choose:** on Game Files, the new **Game code** option comes before the folder: *Recompiled* (as before: the phone builds the code once) or *Native (experimental)*.
- **You can switch:** set up as Recompiled, you can turn native on any time in Video > Native game code (experimental).
- **Status:** it plays from the title screen into the world, with saves and restarts, on 1.9.0 and 1.0.0.

#### Cutscenes at 60 fps (experimental)

### Fixes

- **White water and Link sinking ([#43](https://github.com/mamcjr/BOTWRecompAndroid/issues/43)):** since 0.2.1, some ponds turned white, and Link's legs sank into the ground while he stood still.
- **1.6.0 wouldn't open after building ([#56](https://github.com/mamcjr/BOTWRecompAndroid/issues/56), [#60](https://github.com/mamcjr/BOTWRecompAndroid/issues/60)):** the error was `cannot locate symbol "__ubsan_handle_type_mismatch_v1"`.

### Known issues

- On a hit, the horizon's fog and Death Mountain's cloud sometimes stretch into spikes for one frame.
- The short grass near Link sometimes disappears; under investigation.
- Sometimes, right after a tower's activation scene, the terrain shows stretched into "curtains".
- The water's reflection follows the camera when Link walks sideways.
- On Qualcomm's stock driver, the pause menu's background is black.

## v0.2.1-beta (2026-10-27)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.2.1-beta

**Beta 14.**A correction of the brakes of the 0.2.0-beta.
- **Locked during the game:** the game would be a small lock every few seconds, as if it were carrying something.
- The home screen shows the version of the app in the bottom right corner.
- With Sync lag on, Link's legs can sink to the ground when it's stopped (Video > Sync lag shuts off).
- The grass is low near Link sometimes disappears; in investigation.
- Sometimes, right after the tower's activation scene, the terrain appears stretched in "cortinas".
- The reflection in the water accompanies the camera when Link walks sideways.
- In the Qualcomm factory driver, the bottom of the pause menu is black.

## v0.2.0-beta (2026-10-27)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.2.0-beta

**Beta 13.** A big update: the DLC, version 1.9.0 of the game, save export and import, a game-code build that no longer dies near 80%, and a fix for a crash on the way to Kakariko.
- **DLC (#34, #40, #42):** the Expansion Pass, with The Master Trials (Master Mode, the Master Sword trial, the outfits and masks) and The Champions' Ballad, on versions 1.6.0, 1.8.2 and 1.9.0.
- **Game version 1.9.0 (#28):** the game's last update now runs, beside 1.0.0, 1.6.0 and 1.8.2, each with its own saves.
- **Saves (#40, #49):** in **Settings > Saves**:
- **The build that stopped near 80% (#25, #47, #51):** three things, all fixed:
- **The mesh of dark lines on the ground (#21):** the grid of dark dots over the grass and terrain, which showed while the camera moved and got denser at higher resolutions, came from cracks between the terrain's pieces.
- **Crash on the way to Kakariko (#46, #48):** "no function at 10109c954": a jump inside one of the game's functions that the generator didn't recognize.
- **Faster:**
- **Anti-aliasing: FXAA**, besides SMAA (Video > Anti-aliasing).
- **Controls:** a dead zone for controllers with stick drift (Settings > Controls, with a controller connected), and the gyroscope's drift at rest is taken off (the aim no longer slides).
- **Adreno 7xx (#31):** with Turnip, the game's memory wasn't tracked and everything was copied every frame (~150 MB).
- **Mali-G2-Ultra (#44):** the menus' text went missing; fixed with the settings the issue's author found.
- **Setup:** a file provider that lists everything twice (Huawei, #31, #38) no longer doubles the game image, and picking the folder again no longer deletes the game code built so far.
- **For bug reports:** the crash report now shows which of the game's functions called the one that failed (#24, #30), and the movies log how far they got (#22).
- **Menu language:** the app's menus now follow the game's **Language** option (on Auto, the phone's language).
- The app declares itself a game to Android (game mode, game dashboards).
- With Sync lag on, Link's legs can sink into the ground while he stands still (Video > Sync lag turns it off).
- The short grass near Link sometimes disappears; being investigated.
- Sometimes, right after a tower's activation cutscene, the terrain shows stretched "curtains".
- The water's reflection follows the camera when Link walks sideways.
- On Qualcomm's stock driver, the pause menu's background is black.

## v0.1.11-beta (2026-10-26)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.1.11-beta

**Beta 12.** A lighter game-code build, menus in 11 languages and FPS-style touch controls:
- **Building the game code (#18, #25):** a new compiler starts only while there is free memory, and on a hot phone half of them run; if the app closes midway, the next start goes on where it stopped instead of starting over.
- **Setup (#22, #23):** a `romfs` folder from another version than the `exefs`, or with files missing, is refused before anything is copied, saying what it found.
- **Resolution:** the game's own dynamic resolution is off, and the scene always renders at full resolution.
- **Menus in the game's languages:** Spanish, French, German, Italian, Dutch, Russian, Japanese, Korean, and Simplified and Traditional Chinese, besides Portuguese and English, by the phone's language.
- **Touch controls (Controls tab):** the camera by dragging on the right half of the screen, with a sensitivity setting; sticks shown only while touched; larger stick areas.
- **Quit > Return to start** closed the app on Adreno 8xx; fixed.
- In free aspect, the leaves going dark for a frame or two came from the game's resolution changes, which are now off; not yet confirmed gone.
- Sometimes, right after the tower's cutscene, "curtains" of stretched terrain show and Hyrule Castle's spires stretch up to the sky; being investigated.
- The water's reflection follows the camera when Link walks sideways.
- On Qualcomm's stock driver, the pause menu's background is black.

## v0.1.10-beta (2026-10-25)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.1.10-beta

**Beta 11.** The game's videos now show:
- **Videos (pre-rendered cutscenes, #22):** the Great Plateau Tower's video showed only colored noise while the sound played, and on an Adreno 840 the game froze when the video started.
- In free aspect, tree leaves still go dark for a frame or two when the game's dynamic resolution changes.
- Sometimes, right after the tower's cutscene, "curtains" of stretched terrain show around it and Hyrule Castle's spires stretch up to the sky.
- The water's reflection follows the camera when Link walks sideways.
- On Qualcomm's stock driver, the pause menu's background is black.

## v0.1.9-beta (2026-10-25)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.1.9-beta

**Beta 10.** Motion controls and fixes from the issues:
- **Motion controls (new):** the phone's gyroscope, or a controller's if it has one, aims the bow, the scope and the Sheikah Slate's camera, and tilts the shrines' motion puzzles, as on the Switch.
- **Chinese (#15):** choosing Chinese (Simplified) or (Traditional) showed the game in Japanese.
- **Mali (#1, #19):** drivers that make more than 8 screen images (a Mali-G57 made 12) closed the game at start with "renderer supports at most 8".
- **Setting up the game files (#17):**
- **Building the game code on the phone:** back to 0.1.7's order.
- **Driver error (#13):** the dialog starts by saying what to do, in the phone's language, before the technical details.

## v0.1.8-beta (2026-10-25)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.1.8-beta

**Beta 9.** Qualcomm's own driver works again, and more older drivers are accepted:
- **Qualcomm's stock driver (no Turnip):** in every beta so far, the game lost the GPU a few seconds after starting (VK_ERROR_DEVICE_LOST), right at the title screen.
- **Older Vulkan drivers (for example, Mali on older drivers):** the game no longer requires `synchronization2`, `extended_dynamic_state` or 16-bit storage; each has a fallback when missing.
- **A way out of driver errors:** when the game stops with a Vulkan error, a dialog offers **Install a driver** (the app restarts with it), **Use the system driver** and **Close**.
- **Exynos (Xclipse):** importing the game's memory into the GPU is off on that GPU, because its first work submission failed.
- **Opening with the screen locked:** the app waits up to 5 s for its window instead of closing with an error.
- **Building the game code on the phone:** the largest files start first, so the build doesn't end waiting on one big file alone.

## v0.1.7-beta (2026-10-25)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.1.7-beta

**Beta 8.** Why a setup failed:
- **"Setup didn't finish"** now shows the cause the app found underneath, for example a file in the picked folder that couldn't be read.

## v0.1.6-beta (2026-10-25)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.1.6-beta

**Beta 7.** More drivers accepted and clearer errors:
- **Qualcomm's driver on Adreno 7xx:** the game stopped with "missing: VK_EXT_extended_dynamic_state2".
- **A chosen driver that doesn't work on the GPU:** the app used to fall back to the system driver silently, and the error read as if Turnip lacked the extensions.
- **Name of the driver in use:** Settings > Files > Vulkan driver shows the package's name, plus the name and version the driver reports itself (for example, "turnip Mesa driver - Mesa 26.3.0").
- **Driver packages:** `.adpkg` is accepted besides `.zip` and `.so`, and `meta.json`'s `libraryName` picks the right library in packages that have several.
- **Incomplete romfs.bin:** an interrupted copy of `romfs.bin` made the game close at start with an unexplained `svcBreak`.

## v0.1.5-beta (2026-10-24)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.1.5-beta

**Beta 6.** Choose the Vulkan driver before the game starts:
- **New "Choose driver" shortcut** (long-press the app icon): it opens the system's file picker to choose a Vulkan driver (.zip), then opens the app with it.
- **The driver-too-old messages** now point to that shortcut.

## v0.1.4-beta (2026-10-24)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.1.4-beta

**Beta 5.** A fix for Android 11 and 12:
- **The app closed at launch on Android 11 and 12** ([issue #3](https://github.com/mamcjr/BOTWRecompAndroid/issues/3)): it used a Java method that only exists on Android 13 and newer.

## v0.1.3-beta (2026-10-24)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.1.3-beta

**Beta 4.** More phones can start the game:
- **Vulkan drivers without `VK_KHR_dynamic_rendering`** (some Mali GPUs and older drivers, see [issue #1](https://github.com/mamcjr/BOTWRecompAndroid/issues/1)): the app stopped at launch with "This phone's Vulkan driver is too old for the game".
- **The driver-too-old message** now lists every missing extension, not just the first.

## v0.1.2-beta (2026-10-24)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.1.2-beta

**Beta 3.** Fixes, mostly for 1.8.2:
- **1.8.2 sound:** the audio renderer read voices in the old layout, so most sounds never played.
- **New game stuck at the Shrine of Resurrection's door** (1.8.2): Zelda's line ("Now, go...") stayed on screen forever because its voice never ended.
- **Crash while loading** (`svcBreak`, now and then): two threads opening files at once got the same file system object.
- **Birds, smoke and mist far off frozen** until Link moved: the game's occlusion test always said "hidden".
- **Quick menu (weapons, shields, bows)** with a black background: the world shows behind it again.
- **Free aspect (1.8.2), pause menu:** the blurred world fills the screen, the inventory cursor sits on the right item, and the black box on the right is gone.
- **Performance overlay:** a new compact style (Video > Overlay style), and it can be dragged with a finger.
- **Menu on a touch screen:** the first tap on an option with arrows (Internal resolution, for example) shows the arrows instead of changing the value right away.

## v0.1.1-beta (2026-10-24)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.1.1-beta

**Beta 2.** Change one thing from 0.1.0: if the folder chosen in **Configure game files** bring the game code already compiled to this version of the app (`botwgame.so`, `botwgame.so.id` and `botwgame.so.game`, with the game version), the app uses this code instead of compiling on the mobile phone. Without it, the app compiles on the phone as before, which **has not yet been tested with BOTW**.

## v0.1.0-beta (2026-10-24)

Upstream: https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.1.0-beta

**First beta.** Tested on one phone only: POCO F7 Ultra (Adreno 830, Mesa Turnip driver). **No game files are included**: you need your own extracted dump (`exefs` + `romfs`) of version 1.0.0, 1.6.0 or 1.8.2.
- Playable from the title screen into the world, with saves: 1.0.0 and 1.6.0 at 30 fps, 1.8.2 at 30 or up to 60 (about 40-47 fps in the open world with Turnip).
- Known gaps: setting the game up in the app compiles the game code on the phone, **not yet tested or timed with BOTW**; non-Adreno-8xx GPUs take slower paths; black thumbnails for saves made in the app; simplified audio mixer.
- Android 11+ (arm64, Vulkan), about 30 GB free per game version.

#### Créditos / Credits
