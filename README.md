# EmuCoreA

[![Support EmuCoreA on Patreon](https://img.shields.io/badge/Patreon-Support%20EmuCoreA-ff424d?logo=patreon&logoColor=white)](https://www.patreon.com/c/emucore/membership)
[![Join the EmuCoreA Discord](https://img.shields.io/badge/Discord-Join%20the%20server-5865F2?logo=discord&logoColor=white)](https://discord.com/invite/c5EBeNRpz2)
[![Website](https://img.shields.io/badge/Website-emucorea.web.app-1f6feb?logo=googlechrome&logoColor=white)](https://emucorea.web.app/)
[![Get it on Google Play](https://img.shields.io/badge/Google_Play-EmuCoreA-414141?logo=googleplay&logoColor=white)](https://play.google.com/store/apps/details?id=com.sbro.emucorea)

EmuCoreA is a PSP library, launcher, and emulator frontend for Android. It pairs a purpose-built Compose interface with a vendored [PPSSPP](https://github.com/hrydgard/ppsspp) core that is built together with the app, so no separate core download is needed.

Official website: [https://emucorea.web.app/](https://emucorea.web.app/)

Download it on Google Play: [EmuCoreA on the Play Store](https://play.google.com/store/apps/details?id=com.sbro.emucorea)

![Status](https://img.shields.io/badge/Status-Active%20Development-blue)

The project is under active development. Use your own legally obtained games. PPSSPP emulates the PSP system without a BIOS file.

## Highlights

- PPSSPP-based emulation core built together with the app for ARM64 devices
- Vulkan, OpenGL ES, and software rendering with PSP internal resolution controls
- Game library with PSP title and ID extraction, cover art, search, and per-game settings
- Home screen with shelves, recently played titles, and quick resume
- In-game overlay with rendering, speed, and save state controls
- Touch controls with a layout editor, plus physical gamepad support
- Save states, PSP savedata management, and memory stick size settings
- Cheat and replacement texture catalogs for supported PSP games
- RetroAchievements and optional Discord integration
- Localized interface in 18 languages for phones, tablets, and Android TV

## Screenshots

In-game captures running on a Snapdragon 8 Elite Gen 5 device with the Vulkan renderer:

| Gran Turismo | Ben 10: Protector of Earth |
| --- | --- |
| ![Gran Turismo](Screenshot/gran-turismo.jpg) | ![Ben 10: Protector of Earth](Screenshot/ben-10-protector-of-earth.jpg) |

| GTA: Vice City Stories | Tekken: Dark Resurrection |
| --- | --- |
| ![GTA: Vice City Stories](Screenshot/gta-vice-city-stories.jpg) | ![Tekken: Dark Resurrection](Screenshot/tekken-dark-resurrection.jpg) |

| God of War: Ghost of Sparta |
| --- |
| ![God of War: Ghost of Sparta](Screenshot/god-of-war-ghost-of-sparta.jpg) |

## What This Repository Contains

This repository contains the Android application, its Kotlin UI, the JNI frontend, the vendored PPSSPP sources, and the Gradle module that builds the emulation core for Android. No games, save data, or account credentials are included.

## Tech Stack

- Kotlin + Jetpack Compose
- Android DataStore and Room
- JNI bridge to native C++ built with CMake and the Android NDK
- Vendored PPSSPP Core/GPU/Common built directly into `libemucorea_core.so`
- Vulkan and OpenGL ES rendering paths with a librashader `.slangp` shader runtime
- RetroAchievements integration through rcheevos
- Optional Discord Social SDK integration

## Current App Scope

EmuCoreA version `0.0.5` currently targets Android with:

- `minSdk 26` (Android 8.0)
- `targetSdk 37`
- package id `com.sbro.emucorea`
- version code `18`
- ARM64 devices only

## Core Integration

The vendored core includes PPSSPP `master` through [`bd04d064a4`](https://github.com/hrydgard/ppsspp/commit/bd04d064a4), synchronized on 2026-10-08. EmuCoreA retains its frontend lifecycle, audio queue, surface recovery and release diagnostics patches. Module-start callbacks are also cleared at kernel shutdown so a translation-patch loader cannot leave a guest address active in the next game.

`CoreRuntime` owns the process-wide session and frame worker. `NativePpsspp` calls the native frontend through JNI; the frontend uses PPSSPP's asynchronous loader, JIT/IR interpreter, GPU backends, frame timing, save states and stereo resampler directly. The build excludes the libretro wrapper and PPSSPP's application UI.

| Renderer | PSP rendering | Android presentation |
| --- | --- | --- |
| Vulkan | PPSSPP Vulkan GPU backend | Vulkan swapchain |
| OpenGL ES | PPSSPP GLES GPU backend | Dedicated EGL render thread |
| Software | PPSSPP CPU rasterizer, fixed at PSP resolution | Vulkan swapchain |

Software rendering still needs a working Vulkan driver for presentation and is generally slower than hardware rendering. PSP exposes one local controller; multiplayer uses PSP WLAN/ad hoc rather than a second local controller port.

Global defaults and per-game options are resolved before the loader starts. Per-game overrides survive renderer changes without overwriting global settings. A live renderer change uses a temporary save state, recreates the session and restores its pause state.

Audio uses PPSSPP's stereo resampler with AAudio by default and OpenSL ES as a fallback or explicit choice. Device audio starts after successful boot, stops while paused or without a surface, and is suspended while state restoration resets the sample queue. Output reconfiguration resets the queue before callbacks resume. The device sample rate is independent of the PSP mixer rate.

RetroArch `.slangp` presets run through librashader on Vulkan, OpenGL ES 3 and Software's Vulkan presentation. Presets can be changed during a session. Invalid or unsupported presets fall back to an unfiltered image. GLES 2 devices use direct presentation. Individual presets can require shader features unavailable on a device or backend.

## Building Locally

### Requirements

- Android Studio with Android SDK and NDK configured
- JDK 17
- Android SDK 37 and Android NDK `29.0.14206865`
- CMake `3.30.5`
- For `.slangp` shaders: Cargo/Rust with the `aarch64-linux-android` target; the native build fetches a pinned librashader revision. Without Cargo, the core builds without the shader runtime.

Configure `sdk.dir` in your untracked `local.properties`. The app also needs its Firebase `app/google-services.json`; account credentials, release signing configuration and the optional Discord SDK are not distributed in this repository.

### Debug Build

```powershell
.\gradlew :app:assembleDebug
```

### Release Build

```powershell
.\gradlew :app:assembleRelease
```

Install `app/build/outputs/apk/debug/app-debug.apk` on an ARM64 Android device.

Release builds exclude the device-test host, native inspection JNI, audio captures, frame diagnostic counters and PPSSPP web-debugger assets. Frontend logs and PPSSPP log calls/debug assertions are compiled out for the EmuCoreA production core; runtime error handling remains active. `python app/src/test/python/verify_release_apk.py <release-apk> --aapt2 <SDK-aapt2-path>` checks the packaged DEX/native library, assets and manifest for frontend debug artifacts and the `debuggable`/`testOnly` flags.

### Optional Discord SDK

Discord support is built when a compatible Discord Social SDK directory is supplied through `emucorex.discord.sdkDir` in `local.properties`, a Gradle property with the same name, or `DISCORD_SDK_DIR`. The directory must contain `include/discordpp.h`, `arm64-v8a/libdiscord_partner_sdk.so`, and `discord_partner_sdk.aar`. The SDK is not included in this repository.

## Verification

```powershell
.\gradlew :app:testDebugUnitTest :app:lintDebug :app:assembleDebug
```

With one ARM64 device selected through `ANDROID_SERIAL`:

```powershell
.\gradlew :app:connectedDebugAndroidTest
```

`NativeCoreLifecycleInstrumentedTest` generates PSP ELF programs locally and exercises all three renderers with JIT and IR, live CPU changes, surface resize/replacement, abandoned-window recovery, audio pause and backend changes, save-state restoration, rewind release, guest exit, invalid guest execution and failed-boot recovery. Separate guests present a CPU-written framebuffer and draw through real GE display lists, including hardware rendering with buffer effects skipped. Tests use temporary storage and do not require game downloads.

The optional real-game test accepts an instrumentation `game` argument containing an already accessible path or SAF document URI; it checks presentation with shaders disabled and enabled using a generated identity preset. By default this checks startup only. Gameplay measurements require navigating to an actual playable scene and visually confirming it; logos, movies and menus are not gameplay benchmarks.

For scripted navigation, `gameActions` accepts comma-separated button/wait durations such as `cross:3,start:3,wait:10`. `reuseNavigationState=true` saves the navigated scene and restores it for the other renderers within the same test. `stageSeconds` sets the measurement duration, `gameplayInput=true` sends combat/movement inputs, and `expectMovingScene=true` checks that captured frames change. Navigation depends on the game's menus and save data. Screenshot collection pauses emulation and audio outside the measured interval; those pauses are diagnostic overhead, not gameplay performance. Optional `captureAudio=true` exports PCM from a debug build with `debug.emucorea.audio_capture=1`; restore that property after testing.

Visible output is checked from the composited display: PSP framebuffer alpha stores stencil, so interpreting a raw surface capture as premultiplied Android pixels can incorrectly turn valid RGB into transparent black.

Additional native checks:

- `core-native/src/test/cpp/run-device-tests.ps1 -DeviceSerial <adb-serial> -Configuration Release`: after `:app:assembleRelease`, compiles ARM64 probes against the packaged core and runs the module callback, stereo resampler, upstream GE arithmetic and spline tessellation suites on the selected device. Temporary device files are cleaned afterward; the installed app and its data are untouched.
- [Vulkan presentation contracts](app/src/test/cpp/vulkan-presentation/README.md): execute the production presenter against controlled Vulkan dispatch, including synchronization and resource lifetime.
- [GPU color probes](app/src/test/cpp/shader-colors/README.md): device EGL/Vulkan checks against the production shader runtime.
- [Stereo resampler regression](core-native/src/test/cpp/stereo_resampler_test.cpp): links the actual native core and checks stereo order through underruns, queue resets and conversion to 44.1/48/96 kHz output.
- [Release diagnostic evaluation](core-native/src/test/cpp/release_diagnostics_test.cpp): compile with C++17, `-DNDEBUG` and `-I core`, both with and without `-DEMUCOREA_RELEASE=1`; verifies that production logs/debug assertions do not evaluate their arguments while the enabled control build still does.

These checks cover integration behavior. Game compatibility, sustained performance and audible output still depend on the title, device, driver and selected settings.

The 2026-10-08 upstream sync passed 163 JVM tests, 21 Android tests on a OnePlus CPH2747, native Vulkan contracts, release artifact checks and the device native suites above. A separate Tekken 6 Arcade fight was visually confirmed on all three renderers with shaders off, identity and off again. These short gameplay checks verify rendering and state restoration rather than sustained performance.

## Project Structure

- `app/` Android application, Kotlin UI, unit tests and device tests
- `core-native/src/main/cpp/` JNI frontend, graphics/audio integration and shader presentation
- `app/src/main/res` Android resources and translations
- `core/` Vendored PPSSPP sources
- `core-native/` Gradle module that builds `libemucorea_core.so` for Android
- `tools/` Local release, catalog, and cover tooling (not part of the app build)

## Supported Content

ISO, CSO, and CHD are the main game image formats. PBP, ELF, and PRX support depends on the content and the bundled core. For ISO and PBP, the library can read the title, ID, and icon from PSP metadata. The bundled PSP catalog supplies additional cover art. No games, save data, or account credentials are included here.

## Content Catalogs

- [PSP cheats](https://github.com/sashkinbro/EmuCoreA-Cheat)
- [PSP texture packs](https://github.com/sashkinbro/EmuCoreA-Textures)

The catalogs credit the original sources. Redistributable files are mirrored in their respective catalog releases; entries without redistribution permission link to their authors' downloads.

## Notes

- Game images, save data, and account credentials are not distributed with this project.
- Compatibility, performance, and graphics behavior vary by game, device, renderer, and driver stack.
- Releases marked as "parallel" are identical to the primary build but use an alternate package ID, so they can be installed side by side.

## Credits and license

EmuCoreA builds on PPSSPP. The root [LICENSE.TXT](LICENSE.TXT) is an exact copy of PPSSPP's upstream license file. The vendored core and its dependencies retain their copyright and license notices in `core/`.

Thanks to the PPSSPP contributors and to the RetroAchievements team for rcheevos.

EmuCoreA is independent of Sony, PPSSPP, IGDB, Discord, and RetroAchievements. PSP is a trademark of Sony Interactive Entertainment. Game artwork and game data belong to their respective owners.

## Support

If you want to support ongoing development:

- Website: [https://emucorea.web.app/](https://emucorea.web.app/)
- Patreon: [https://www.patreon.com/c/emucore/membership](https://www.patreon.com/c/emucore/membership)
- Discord: [https://discord.com/invite/c5EBeNRpz2](https://discord.com/invite/c5EBeNRpz2)
- More apps by the author: [Google Play developer page](https://play.google.com/store/apps/dev?id=7136622298887775989)
