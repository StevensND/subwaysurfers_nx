<div align=center>

<img src="extras/banner.png" alt="Banner" width="35%">

</div>
<h1 align=center>Subway Surfers — Nintendo Switch port</h1>

A wrapper/port of the Android release of Subway Surfers (v3.66.1). It loads the
original game binaries (`libmain.so`, `libunity.so` and `libil2cpp.so`, Unity
2022.3 IL2CPP, arm64), resolves their imports against native Switch
implementations and patches them so the game runs inside a minimal Android
environment.

## How to install

Create a folder for the game on your SD card, `/switch/subwaysurfers_nx/`, and
place:

1. `subwaysurfers_nx.nro`.
2. The complete contents of a Subway Surfers 3.66.1 arm64 APK. Open the APK with
   7-Zip or another ZIP extractor and extract it directly into the folder.

```text
/switch/subwaysurfers_nx/
  subwaysurfers_nx.nro
  lib/
    arm64-v8a/
      libmain.so
      libunity.so
      libil2cpp.so
  assets/
    bin/Data/ ...
  cursor.png                <- optional, replaces the mouse-mode cursor
```

Optionally drop a `cursor.png` (up to 64×64, transparency respected) in the same
folder to replace the on-screen cursor with your own.

The first launch can take several minutes. The port validates the extracted
game, removes unused Android files and converts the loose assets into an
optimized pack to avoid long loading time.

Launch with a game override (hold R while starting a title) or a forwarder with
full application memory. Album applet mode does not provide enough memory or
the required code-memory permissions.

## Controls

Subway Surfers is driven by swipe gestures, so the analog sticks and D-pad are
mapped to synthetic swipes and the face/shoulder buttons tap the on-screen
controls. The touchscreen still works in handheld mode.

| Input | Action |
| --- | --- |
| D-pad / Left stick / Right stick — Up | Jump (swipe up) |
| D-pad / Left stick / Right stick — Down | Roll (swipe down) |
| D-pad / Left stick / Right stick — Left / Right | Change lane (swipe left / right) |
| A | Tap centre (confirm in menus) |
| B | Android back |
| X | Score Booster power-up |
| Y | Headstart power-up |
| R | Hoverboard (double-tap) |
| − or + | Pause |

A new direction fires immediately and interrupts an in-progress swipe, so you
can change lanes at the last moment to dodge a train. Holding a direction does
not repeat, so a single push is a single swipe.

### Mouse mode

Press **ZL** to toggle an on-screen cursor for navigating menus, shops and
pop-ups. While the cursor is active:

| Input | Action |
| --- | --- |
| ZL | Toggle the cursor on / off |
| Left stick / Right stick | Move the cursor |
| A or ZR | Click at the cursor |
| D-pad — Up / Down | Adjust cursor speed (hold to keep changing it) |

While the cursor is on, both sticks and the D-pad drive it instead of the
character, so they never register as swipes. Each press of D-pad Up / Down nudges
the cursor speed up or down, and holding either one keeps adjusting it. The cursor
draws over dimmed pop-ups, so you can move to the close button and click it.

A USB mouse works in both handheld and docked: move to control the cursor,
left-click to tap, and use the scroll wheel to change sensitivity. Your stick and
mouse sensitivities are remembered in `pointer.cfg` automatically after in-game
adjustment.

## Build

devkitA64 plus these portlibs:

```sh
pacman -S switch-mesa switch-libdrm_nouveau switch-sdl2 switch-zlib
```

Then `make` from a devkitPro shell.

A GitHub Actions workflow (`.github/workflows/build.yml`) is also included: it
builds the `.nro` on every push using the `devkitpro/devkita64` image and can
publish it as a release on manual dispatch. A guard step refuses to build if any
game binary (`.so`, APK, `dump.cs`, `global-metadata`) is ever committed, to
avoid redistributing copyrighted code.

## Credits

- TheOfficialFloW & Andy Nguyen — the original Android so-loader.
- fgsfds — the Switch so-loader groundwork reused here.

### Support

If you enjoy my work and want to support me :

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)

## Legal

No affiliation with SYBO Games or Kiloo. "Subway Surfers" is a trademark of
its owner. This repository contains no assets or program code from the original
game, and none may be distributed with builds. Users must extract the required
files from their own legally obtained copy.

Source code is provided under the MIT License (see LICENSE).
