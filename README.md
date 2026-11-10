# Dusklight Co-op — online co-op Twilight Princess for Android and PC

**Dusklight Co-op** is a fork of [Dusklight](https://github.com/TwilitRealm/dusklight), the reverse-engineered
PC/Android port of *The Legend of Zelda: Twilight Princess*, with online two-player co-op **built in**.
No separate mod to install: create a room, give your buddy the Room ID and password, and play the story together.

Keywords: Zelda Twilight Princess co-op, Twilight Princess multiplayer, Twilight Princess online, Dusklight multiplayer,
Dusklight co-op, Dusk TP port, Zelda co-op Android APK, two player Zelda, online co-op Zelda, GameCube decomp port.

> Dusklight does **not** include any copyrighted game assets. You must provide your own legally dumped copy of
> the original GameCube game, exactly as with the official Dusklight.

## Features

- **Rooms with a Room ID and password.** Open the **Co-op Online** tab in the menu, choose *Create room*, type a
  Room ID and a password (6–128 characters), and share them. Your buddy picks *Join room* and enters the same
  Room ID and password. No port forwarding needed.
- **Shared enemy HP (on/off).** When either player damages an enemy, the same enemy in the other player's game loses
  the same HP, so you wear enemies down together. The killing blow always plays the game's normal defeat animation
  and drops.
- **Two-player bosses.** With Shared HP on and both players in the stage, boss damage is halved on each hit, which
  doubles every boss's health for the two of you.
- **Boss doors need both players (on/off).** While you're in a room, a boss door only opens when your partner is
  standing at it too. Arrive alone and an on-screen message tells you to wait for your partner.
- **Hidden Skills unchanged.** The Hero's Shade sword-skill lessons unlock one at a time as normal and never need two
  players.
- Everything from the MFB Multiplayer mod: synced story progression, items, keys, switches, chests, heart pieces,
  remote Link models, chat, optional PvP and voice chat (PC).

The room **host's** co-op settings are used for everyone, so both games always agree.

## Install (Android)

1. Download `DusklightCoop-…-android-arm64.apk` from the [Releases](../../releases) page.
2. Allow installing from your browser, then open the APK. It installs **next to** official Dusklight
   (app name *Dusklight Co-op*), so your existing install is untouched.
3. Point it at your own game dump, exactly as in official Dusklight.

Both players must run Dusklight Co-op (the same version).

## What is and isn't synced

Enemies are not position-synced: each of you sees enemies move in your own game. What is shared is their **health**,
plus all story progression. Some bosses track damage with their own hit counters instead of health; those still
work normally but may not get the full double-health effect.

## Building

The GitHub Actions workflow (`.github/workflows/coop.yml`) runs the co-op unit tests with sanitizers, builds and
tests the online module on Linux, then builds, signs and verifies the Android arm64 APK. Tagging `v*` publishes a
release. Local builds work exactly like upstream Dusklight; see [docs](docs/).

## Credits and license

- [Dusklight](https://github.com/TwilitRealm/dusklight) by the Twilit Realm team (CC0-1.0).
- [MFB Multiplayer / Dusklight Online](https://github.com/matthewdavidrichardanderson/dusklight-online-mod) by
  mdra5000 (CC0-1.0), bundled in `mods/dusklight_online`.
- Co-op additions: shared enemy HP, two-player boss health, boss-door gate, Room ID/password UI.

This project is not affiliated with or endorsed by Nintendo or the Twilit Realm team.
