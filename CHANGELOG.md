# Changelog

## 0.0.1 (not released)

- The build pipeline: `setup.sh` pins GoldenEye-Recomp, its ReXGlue SDK fork and reKing's PS5 toolchain, builds the
  ReXGlue CLI and recompiles the game from your own `default.xex`; `build-ps5.sh` compiles it with the PS5 compiler
  (listing what still fails), then links and packages it as reKing does.
- The porting plan: `docs/PORTING.md`.
