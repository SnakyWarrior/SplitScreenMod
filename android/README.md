# SplitScreenMod for Android (experimental)

The Windows mod (in `src/`) cannot run on Android: it is built for LeviLamina on Windows x64, and the Android
LeviLauncher only loads Android libraries (`.so`). This folder is a separate mod for **LeviLauncher on Android**,
built with LeviLauncher's [preloader-android](https://github.com/LiteLDev/preloader-android) SDK.

## What this version does

Version 0.4 plays split screen on Android:

- **Player 1 keeps the touch screen.** Press Start on a controller: a new player joins with it (player 2, 3, 4) and
  the screen splits. Start on a controller that already plays opens that player's pause menu. A player that leaves
  can join again with Start.
- Every controller is a controller of its own: the game on Android sends every controller to player 1, the mod gives
  each Android device its own controller number.
- Split screen is on: the game keeps it off on phones it thinks are too small (`AppPlatform::allowSplitScreen`).

Settings (`config/config.json`): `firstControllerIsPlayer1` gives the first controller to player 1 instead of touch.

Not ported from Windows yet: the player picker, the pause menu's Split screen menu (layout, swap, remove), the P1-P4
pictures, and the 20 second disconnect countdown. The mod still logs a lot (it is in development): everything it does
is written to `splitscreen.log` and `Download/SplitScreenMod-android.log`.

## Install

Download the newest build from [releases/android-latest](../../../releases/download/android-latest/SplitScreenMod-android-arm64.zip) (always the same link) and add it in
LeviLauncher's **Mods → Add Mod**. Start the game, enter a world, then send `Download/SplitScreenMod-android.log`.

## Build

Needs the Android NDK (28.2), CMake 3.22+ and Ninja:

```
cmake -S android -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_HOME/ndk/28.2.13676358/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release
cmake --build build-android --target levi_package
```

The package is `build-android/SplitScreenMod-android-arm64.zip` (also as `.levipack`).
