# Real-Time Raytracer

## Build Scripts

From MSVC x64 developer command prompt (or `cmd` with `vcvars64.bat`):

- `build_lib` to compile third-party sources to `out\lib.lib` (only necessary to run once, before first build)

- `build` to compile and link the executable to `out\ratracer.exe`

- `run` to build and run the raytracer (if compilation was successful)

- `run_last` to run the previous successful build

## Matrix Conventions

Unless explicitly stated, all shader and host code uses row-major matrices with premultiplication and row vectors. Both world and camera space use right-handed coordinate systems: World space with Z-up, Y-forward, and X-right; Camera space with Y-up, X-right, and looking towards the negative Z direction.
