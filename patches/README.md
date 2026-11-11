# patches

Applied by `setup.sh`, on clean checkouts:

- `rexglue-*.patch`: changes to the ReXGlue SDK fork (`deps/rexglue`), applied with `git apply`.
- `goldeneye-*.patch`: changes to GoldenEye-Recomp's sources (`work/game`), applied with `patch -p1`.

Keep every change behind `REX_PLATFORM_PS5` so the same tree still builds for Linux. See `docs/PORTING.md`.
