# SplitScreenMod for Android (experimental)

The Windows mod (in `src/`) cannot run on Android: it is built for LeviLamina on Windows x64, and the Android
LeviLauncher only loads Android libraries (`.so`). This folder is a separate mod for **LeviLauncher on Android**,
built with LeviLauncher's [preloader-android](https://github.com/LiteLDev/preloader-android) SDK.

## What this version does

Nothing about the Android game's code is known yet, so version 0.1 is a first step:

1. It finds the game's classes by their RTTI names (`AppPlatform`, `AppPlatform_android`, `MinecraftGame`, ...)
   and writes their virtual functions to `splitscreen.log`. The log is also copied to `Download/SplitScreenMod-android.log`.
2. If the game exports the `AppPlatform::allowSplitScreen` symbol, it turns split screen on, the same switch the
   Windows mod uses.
3. Otherwise it leaves the game alone and lists the likely candidates in the log, so the next version can pick
   the right one.

Controllers, the player menu and the pause menu button of the Windows mod are not ported yet.

## Install

Download `SplitScreenMod-android-arm64.zip` from the Actions tab (the **Android** workflow) and add it in
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
