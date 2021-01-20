# Real-Time Raytracer

## Build Scripts

From MSVC x64 developer command prompt (or `cmd` with `vcvars64.bat`):

- `build_lib` to compile third-party sources to `out\lib.lib` (only necessary to run once, before first build)

- `build` to compile and link the executable to `out\ratracer.exe`

- `run` to build and run the raytracer (if compilation was successful)

- `run_last` to run the previous successful build

- Further MSVC arguments can be supplied to `build`, `build_lib`, and `run`, such as `/Zi`, `/O2`, etc. to control compiler output
