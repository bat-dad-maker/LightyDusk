**Beta 17.** Mali performance back, keeping 0.2.3's fixes, and the game code build that always failed on some phones.

## New

- **One romfs.bin instead of ~41,000 files ([#87](https://github.com/mamcjr/BOTWRecompAndroid/issues/87)):** copying the whole romfs/ folder to the phone is slow and fails on many devices. The picked folder can now hold `exefs/` and a ready-made **`romfs.bin`** beside it (made on a PC, e.g. with `hactool --romfs=romfs.bin`, the update applied with `--basenca`). The app checks its version and that it's whole, and it works with both **Copy them** and **Use them in place**. A romfs/ folder still works as before.

## Fixes

- **Mali slow in 0.2.3 ([#88](https://github.com/mamcjr/BOTWRecompAndroid/issues/88), [#89](https://github.com/mamcjr/BOTWRecompAndroid/issues/89), [#93](https://github.com/mamcjr/BOTWRecompAndroid/issues/93), [#54](https://github.com/mamcjr/BOTWRecompAndroid/issues/54), [#61](https://github.com/mamcjr/BOTWRecompAndroid/issues/61), [#94](https://github.com/mamcjr/BOTWRecompAndroid/issues/94)):** to end the crashes and garbled text, 0.2.3 put the memory the CPU and GPU share in Mali's **uncached** memory type. Every CPU read and write there is very slow, and the world dropped to 2-9 fps. Now that memory goes back to the cached type, and the app syncs the caches by hand. 0.2.3's fixes still hold. Tested on an Adreno set up like Mali's memory; there's no Mali phone to test on: please tell me how it runs.
- **Build failing after a second ([#52](https://github.com/mamcjr/BOTWRecompAndroid/issues/52), [#92](https://github.com/mamcjr/BOTWRecompAndroid/issues/92)):** when the build resumed where it had stopped, it read the previous run's log and asked the compiler for a file that didn't exist yet (`file_open FileNotFound`). Now each run starts with a fresh log and waits for each file to be ready.
- **Gyroscope with a USB-C controller ([#78](https://github.com/mamcjr/BOTWRecompAndroid/issues/78)):** with the GameSir X5 Lite, the gyroscope worked for the first aim and then stopped. One stray reading made the app switch to the controller's gyroscope, which only sends zeros. Now the controller's takes over only after moving for a quarter of a second, and the phone's comes back once the controller's has read zero for 2 s. There's no such controller here to test with: please tell me if it's fixed.

> **No need to build the game code again:** just install over 0.2.x.

## Known issues

- Free aspect: when the menu turns its page, the page shows cut at the 16:9 part's edge while it slides.
- Short grass in the distance only appears up close.
- Sometimes, right after a tower's activation scene, the terrain shows stretched into "curtains". To fix it, load the save once the tower is active.
- The water's reflection follows the camera when Link walks sideways.
- On Qualcomm's stock driver, the pause menu's background is black.
- Native mode: Link sometimes flickers on the first hit he takes.

No game files are included. Tested on one phone only (POCO F7 Ultra, Adreno 830, Turnip). To install and set up: [0.1.0-beta](https://github.com/mamcjr/BOTWRecompAndroid/releases/tag/v0.1.0-beta). Credits in the [README](https://github.com/mamcjr/BOTWRecompAndroid#credits).
