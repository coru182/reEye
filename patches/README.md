# patches

Applied by `setup.sh`, on clean checkouts:

- `rexglue-*.patch`: changes to the ReXGlue SDK fork (`deps/rexglue`), applied with `git apply`.
- `ffmpeg-*.patch`: changes to its FFmpeg submodule (`deps/rexglue/thirdparty/FFmpeg`, its own repository).
- `goldeneye-*.patch`: changes to GoldenEye-Recomp's sources (`work/game`), applied with `patch -p1`.

Keep every change behind `REX_PLATFORM_PS5` (or `__PROSPERO__`, `REXGLUE_PS5` in CMake) so the same tree still
builds for Linux. See `docs/PORTING.md`.

## rexglue-ps5.patch

- `rex/platform.h`: the PS5 (`__PROSPERO__`) as a platform: `REX_PLATFORM_PS5`, on the POSIX/Linux code paths.
- `rex/platform_ps5.h`: FreeBSD has no `*64` file calls (off_t is 64-bit) and no `CLOCK_MONOTONIC_RAW`.
- `rex/chrono/clock_cast_compat.h`: `clock_time_conversion` / `clock_cast`, which libc++ 18 lacks.
- `core/exception_handler_posix.cpp`: FreeBSD's signal context (`mc_rip`, `mc_fpstate`) instead of Linux's `gregs`.
- `core/threading_posix.cpp`: FreeBSD thread names, ids and CPU sets; `pthread_kill` for the wakeup signal.
- `system/xsocket.cpp`: FreeBSD defines `IPPROTO_UDP` as a macro.
- `ui/CMakeLists.txt`, `cmake/rexglue_helpers.cmake`: no GTK, X11 or desktop entry point on the PS5.
- `system/CMakeLists.txt`, `CMakeLists.txt`: rexruntime is a static library on the PS5, and the PS5 build's
  libraries go to `out/ps5-amd64` (apart from the Linux build of the ReXGlue CLI).
- `thirdparty/renderdoc`, `thirdparty/crypto`: their OS checks and `<sys/endian.h>`.
- Vulkan on VideoOut: RADV linked in (`vk_icdGetInstanceProcAddr` instead of a loader), `VK_KHR_display`
  (`instance_khr_display.inc`), a `Ps5DisplaySurface` (`ui/surface_ps5.h`) and the display plane surface in
  `ui/vulkan/vulkan_presenter.cpp`, as reKing does.
- `core/filesystem_posix.cpp`: the app's folder is /data/reeye (no /proc on the PS5).
- Sound: on the PS5 the SDL audio driver plays through `sceAudioOut` (48 kHz 16-bit stereo, one 256-sample frame a
  call) instead of SDL's device, which the PS5's SDL lacks (`audio/sdl/sdl_audio_driver.cpp`).
- Input: the default input system takes reEye's `ps5/input_ps5.cpp` (the DualSense through libScePad) on the PS5.
- Guest memory (`core/memory_posix.cpp`, `system/xmemory.cpp`): the 4.5 GB "file mapping" is one PS5 direct-memory
  object (`ps5platform/shm.h`), mapped at Xenia's views inside one reserved range; commit, decommit and release in
  the views are protection changes on whole 16 KB pages. The 0xE0000000 heap's 4 KB offset is applied as on Windows
  (`xmemory.h`, and the recompiler's `init_h.inja`, so the CLI is rebuilt when the patch changes).
- `cmake/rexglue_helpers.cmake`: the game target takes `REXGLUE_PS5_HOST_SOURCES` (reEye's `ps5/app_ps5.cpp`) in
  place of the GTK entry point.

## ffmpeg-ps5.patch

- `config_ps5_x86_64.h`: FFmpeg's Linux x86-64 configuration without what FreeBSD lacks (`malloc.h`, `memalign`,
  Linux headers).

## goldeneye-ps5.patch

- `src/ge_hooks.cpp`: GoldenEye-Recomp's Windows-only parts (relaunching itself, the raw-mouse window and cursor
  capture, `GetAsyncKeyState`, the watchdog's `VirtualQuery`) are left out on the PS5: no relaunch, no mouse look and
  no keyboard yet, and the diagnostic stack walk is skipped.
