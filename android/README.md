# SplitScreenMod for Android (experimental)

The Windows mod (in `src/`) cannot run on Android: it is built for LeviLamina on Windows x64, and the Android
LeviLauncher only loads Android libraries (`.so`). This folder is a separate mod for **LeviLauncher on Android**,
built with LeviLauncher's [preloader-android](https://github.com/LiteLDev/preloader-android) SDK.

## What this version does

Version 0.1.2 turns split screen on: it finds `AppPlatform::allowSplitScreen` in the game's vtable (slot 105 in
1.26) and makes it answer true. The slot is found from the order of AppPlatform's virtual functions, not from a
fixed number: three pure virtual functions two slots apart (`swapBuffers`, `getSystemLocale`, `pickImage`), then
`allowSplitScreen` 28 slots later, confirmed by the next function `splitScreenRenderDistScalar`.

Everything it does is written to `splitscreen.log`, which is also copied to `Download/SplitScreenMod-android.log`.

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
