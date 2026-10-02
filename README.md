# Defective Engine

A moddable voxel engine for low-end hardware, written in C17 with OpenGL 3.3 core.

## Build and run

Requires CMake, Ninja or Make, a C compiler and the GLFW development files. All other dependencies are vendored.

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/dfe                 # run from this folder so engine_assets and mods are found
./build/dfe --selftest      # engine and example mod checks
./build/dfe --benchmark     # fixed camera path, prints frame statistics
./build/dfe --help          # every option
```

Add `-DDFE_BUILD_EXAMPLES=ON` to build the native plugin example.

## Modding

See [docs/MODDING.md](docs/MODDING.md) for the complete guide and API reference. Example mods are in `examples/mods`.
