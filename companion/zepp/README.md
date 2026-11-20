# Recomp Mapa 0.2.1 — Amazfit GTR 4 New

External map stored on the clock, with Link position and direction, pins/stamps and mission indicators selected sent by paired mobile phone. The main page does not capture or transmit the image of the minimap.

## Correction of interruptions at 0.2.1.

The user reported locking and restarting the clock upon receiving a notification with 0.2.0. The cause of reboot in firmware has not yet been confirmed by logs. The review fixes risks found in the code: cancels timers and releases pause controls, discards responses and callbacks from the previous generation, waits 300 ms before resuming and does not intercept vertical gestures of the system. The transition preserves the position and zoom of the map.

The design reuses no more than 16 image components and the components of the markers, without destroying/recreating components to each sample. Do not redefine the image when only its position changes, nor reloads the tiles from a still map. This limits peak images during pan/zoom and reduces allocations in interruptions.

24 tests have passed, including 150 simulated cycles of interruption/resumption, old success/error responses, cancellation of design, absence of access to the IU during pause/transition, and limit of components. These tests use API mocks; the correction still needs to be validated by receiving a notification in GTR 4 New. Update only Mini Program by QR 0.2.1; APK has not changed. Lifecycle and gestures follow the official documentation of [delegate](https://docs.zepp.com/docs/1.0/watchface/api/hmUI/createWidget/) and [gestos](https://docs.zepp.com/docs/1.0/reference/device-app-api/hmApp/gestureEvent/registerGestureEvent/).

## Installation

1. Instale o APK atualizado de `android/botwrecompandroid/app/build/outputs/apk/debug/app-debug.apk`.
2. On Android, press the Recomp icon → **Map on the clock**. Enable sharing, save, and copy the code.
3. In Zepp developer mode, add Mini Program by QR of previous 0.2.0. Use the target **Amazfit GTR 4 new**(9437441), API 1.0 and screen 480×480. The system version displayed on the watch does not match the API_LEVEL of the catalog.
4. In the Mini Program settings in Zepp, paste the 32-character code. The previous code remains valid after updating the APK.
5. Open the game and Recomp Map on the clock, with Bluetooth connected.

The crown changes between four zoom levels. Drag to explore; **Center Link** resumes follow-up. The blue arrow indicates Link, orange dots represent pins/carimbos and yellow circles represent mission objectives. In this version, the pins and stamps retain position and type in the protocol, but use a common visual symbol.

## Preparar mapa e pacote

The map comes exclusively from the local user dump. Extracted tiles are ignored by Git; do not publish or version the package containing them. The viewer uses levels of 256, 512, 1024 and 2048 pixels, divided into 340 tiles of 128×128.

```powershell
python tools/export_watch_map.py --romfs Dump/01007EF00011E000/romfs --output companion/zepp/assets/gtr-4-new/map
cd companion/zepp
npm ci
npm test
npm run build
node tools/zeus.cjs login
npm run preview -- --target "Amazfit GTR 4 new"
```

The exporter requires Pillow and NumPy. The Zeus 1.9.3 wrapper adjusts the private module resolution and saves the QR matrix to `dist/preview-qr.json`. The scripts exclude intermediate products from the package; the final local compilation has approximately 3.9 MB. Generate the preview sends the package, including the map, to the Zepp service and depends on user authorization. The previous expiry date is indicated by Zeus. `.zab` is the watch package, not an APK. The appId 1202601 is development.

## Ponte e limites

The optional service only listens to `127.0.0.1:18880`, requires code and works with Recomp in the foreground. `/v1/state.json` returns JSON with session, sequence, map, position, direction, markers and objectives. The Side Service on Zepp searches for this state and sends it over Bluetooth. There is a request in flight, nominal interval of a second and pause when the page becomes inactive. The real rate depends on Zepp and the clock. After five seconds without response, the position is identified as outdated.

Native reading specifically supports Switch 1.0.0, built `2094f152836683a3632c6153fd84ab80` revision. Checks instructions for this review, pointer limits and session generations. Uses `pread` only for `/proc/self/mem` reading; does not call game functions in network thread. Position and selection can change between readings, so this is not an atomic motor snapshot. Selection/step are checked again before response. Unknown reviews and uploads return unavailability.

Objectives come from the indicators of the selected mission stage, filtered by the completion flags, instead of using arrays of guides that can store old positions. The coordinates are the positions defined by the mission indicator; following a moving NPC has not yet been validated. Interiors/sanctuaries do not use the external map to design Link.

The capture endpoints `/v1/map.json` and `/v1/map.png` and the old page remain legacy diagnostic; they are not used by the main page. Clipping fields on Android only affect this diagnosis.

## Validation

- Real position confirmed by reading before/after walking on the cell phone (approx. 60 m).
- Installed Android Endpoint returned Link and two bookmarks placed by the user.
- Selected mission `FirstTower` corresponds to **Follow the Sheikah Slate**, confirmed by the user; tower indicator in approximately x=-560, z=1695.
- Final APK installed: after loading the save, `/v1/state.json` returned `objectivesAvailable:true` and the target in x=-560.035, z=1694.863. The two previous test markers did not reappear in this save.
- 23 Node tests have passed, including protocol, validation, coverage of the tiles, pan, zoom, recentralization, goal and discard when closing the page.
- Prior 0.2.0 sent to Zepp with explicit user authorization on 20/10/2026. QR valid until 27/10/2026, in `dist/preview-qr-0.2.0.png`; final package with 340 tiles, API 1.0, about 3.9 MB and no intermediate products.
- The old mirror 0.1.4 was confirmed on the clock by the user. The local map display 0.2.0, gestures and latency still need to be checked in GTR 4 New.

License and legacy code of communication in `shared/legacy`. Communication reference: [exemplo oficial API 1.0](https://github.com/zepp-health/zeppos-samples/tree/main/application/1.0/fetch-api).
