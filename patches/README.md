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
- `thirdparty/renderdoc`, `thirdparty/crypto`: their OS checks and `<sys/endian.h>`.

## ffmpeg-ps5.patch

- `config_ps5_x86_64.h`: FFmpeg's Linux x86-64 configuration without what FreeBSD lacks (`malloc.h`, `memalign`,
  Linux headers).
