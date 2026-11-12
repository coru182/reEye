# Porting notes

What separates GoldenEye-Recomp's Linux build from a PS5 app, found by reading ReXGlue (the
[GoldenEye fork](https://github.com/SunJaycy/GoldenEye-Recomp-rexglue) at `2824a80`, SDK 0.8.0) against what reKing
had to change in its own runtime. `build-ps5/compile-errors.txt` lists what the compiler finds; this lists what it
won't, because most of these compile and then fail on the console.

With the PS5 toolchain file, ReXGlue's CMake takes the Linux path (`REX_PLATFORM_LINUX`). The build adds
`REX_PLATFORM_PS5=1` for the code the port changes. Changes to the SDK go in `patches/rexglue-ps5.patch`, changes to
the game's sources in `patches/goldeneye-ps5.patch`.

## Status

- Compiles: ReXGlue, FFmpeg and the game's own sources pass a syntax check against the PS5 SDK's headers with
  `patches/rexglue-ps5.patch` and `patches/ffmpeg-ps5.patch` (sections 2, 3 and 6 below are done at the source
  level). Next: a full compile on the build machine, then the link.
- Everything compiles; the link needed `main` and three libc functions (`ps5/posix_ps5.c`).
- App shell: `ps5/app_ps5.cpp` (entry point, UI loop, one full-screen window; game files in /data/reeye/game).
- Vulkan: RADV linked in and the VideoOut surface (4) are in the SDK patch.
- Guest memory (1): direct memory through ps5platform's shm (first try, 0.0.1 boots to "Unable to reserve the 4gb
  guest address space" without it).
- Guest memory works on the console (0.0.1: the game loads its files, then the screen stays black).
- Sound (sceAudioOut in the SDL driver) and input (libScePad, controller 1): first try.
- Sound and the DualSense work on the console; the game loads its data and reaches its first frame, then aborts:
  the title heap finds itself damaged (dlmalloc's check, in `mspace_malloc`) at the next allocation, the same place
  every run (RADV's `vkAcquireNextImageKHR` from the presenter's first paint, on the main thread; once on the GPU
  command thread). The damage happens earlier, elsewhere: `ps5/heapcheck_ps5.c` (guard bytes around every block,
  freed blocks kept back and checked, a scan every 2 seconds) is built in to name the block and who allocated it.

## 1. Guest memory (the big one)

`src/core/memory_posix.cpp` builds the 360's address space the Linux way: `shm_open` / `ftruncate` for physical
memory, `mmap(MAP_FIXED)` views of it in several windows, and `/proc/self/maps` to query mappings. On the PS5 an app
may not issue raw syscalls (`SYSTEM_ILLEGAL_FUNCTION_CALL`), there is no `/proc`, and host pages are 16 KiB.

reKing solved the same problem in its `kernel/memory.cpp` (patches/athelas-ps5.patch there): one reserved range
(`ps5_vrange_reserve`), physical memory as one shared object mapped into both windows (`ps5_shm_create`,
`ps5_shm_map`), and commits as 16 KiB host pages taken from 16 MB slabs, four guest pages each. The same design goes
into `memory_posix.cpp` under `REX_PLATFORM_PS5`, with the mapping table kept in memory instead of read from `/proc`.

## 2. Write watches and the exception handler

Xenia's GPU code (shared memory, texture cache) protects guest pages and catches the faults to learn what the game
wrote: `src/core/exception_handler_posix.cpp` and `seh_posix.cpp` install `SIGSEGV` / `SIGBUS` / `SIGILL` handlers and
read registers from `ucontext_t` in Linux's layout. To check on the console first: whether an app's signal handlers
run at all. If they do, the register access needs FreeBSD's `mcontext_t` (`mc_rip`, `mc_rax`, ...). If they don't,
the watches have to become explicit invalidation (slower, but some Xenia forks do it).

## 3. Threads

`src/core/threading_posix.cpp`: `sched_setaffinity` and `syscall(SYS_gettid)` are Linux only (FreeBSD has
`cpuset_setaffinity` and `pthread_getthreadid_np`), and thread suspension uses real-time signals. reKing's
`kernel/threads.cpp` changes are the reference for what the PS5 allows.

## 4. Vulkan

The fork already prefers Vulkan on AMD GPUs, which is what the PS5 has (RADV). ReXGlue loads Vulkan through volk
(`dlopen`); on the PS5 RADV is linked in whole, so volk is started with `volkInitializeCustom` and RADV's
`vk_icdGetInstanceProcAddr`, as reKing's `gpu/vk_loader.cpp` does. The window surface comes from VideoOut (reKing's
`gpu/vk.cpp`) instead of SDL3. The backend's required extensions (fragment shader interlock or the render-target
path for EDRAM) need checking against what PS5 RADV exposes. Note GoldenEye-Recomp's own warning that AMD GPUs may
crash or fail to boot: that is the path the PS5 takes.

## 5. Sound, input, app shell

- Sound: ReXGlue's SDL3 audio driver out, `sceAudioOut` in (reKing's `ps5/audio_ps5.cpp`). XMA decoding uses FFmpeg
  (the `FFmpeg` submodule), which has to build for the PS5.
- Input: SDL3 gamepads out, libScePad in (reKing's `ps5/input_ps5.cpp`), mapped to the 360 pad.
- App shell: `REX_DEFINE_APP` assumes a desktop window. The PS5 entry (`ps5/native_start.c`), the first-start checks
  (game files in `/data/reeye/game`) and the in-game menu come from reKing.

## 6. Smaller things

- `filesystem_posix.cpp` reads `/proc/self/exe` for the executable's folder: the PS5 build uses `/data/reeye`.
- `dynlib_posix.cpp`: `dlopen` (only reached for Vulkan and Android code paths).
- The link checks that no raw `syscall` instruction is left in the program (build-ps5.sh), and `ps5/posix_ps5.c`
  replaces the libc functions that would issue one, as in reKing.

## Order of work

1. Compile everything with the PS5 compiler; empty `compile-errors.txt`.
2. Link; no raw syscalls.
3. Boot to the game's first frame with logging to `/data/reeye/log.txt` and no picture.
4. Picture (Vulkan on VideoOut), then input, then sound.
