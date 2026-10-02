# Defective Engine

A moddable voxel engine for low-end hardware, written in C17 with OpenGL 3.3 core. The base game is itself a mod.

The priorities, in order, are low-end performance, deep moddability and atmosphere. The reference machine is a 2015 dual-core laptop with an Intel HD 520 and 4 GB of RAM.

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

Use a Release build for any performance measurement. Add `-DDFE_BUILD_EXAMPLES=ON` to build the native plugin example.

## Playing

Starting `dfe` with no world option opens the title screen. It lists the folders in `saves/`, and New World asks for a name and an optional seed. An empty seed is random, a number is used as given, and any other text is hashed, so a word always gives the same world. Passing `--world NAME` skips the title screen.

| Input | Action |
|-------|--------|
| W A S D, Space, Left Shift, Left Ctrl | Move, jump or swim up, descend, sprint |
| F | Toggle flight (creative mode) |
| Left, right, middle mouse | Break, place, pick block |
| E | Inventory |
| Esc | Pause menu: Resume, Settings, Save and quit. Esc again resumes. |
| Grave key | Console (`help` lists commands) |
| F3, F4, F5 | Debug overlay page, wireframe, reload shaders and data |

The world and the day cycle stop while the pause menu is open.

### Settings

Settings are available from the title screen and the pause menu and are saved to `settings.json` in the working directory. Each change applies immediately.

| Setting | Meaning |
|---------|---------|
| Preset | Low, Medium or High, defined by data files (see the modding guide). A preset sets render distance, far terrain, clouds, stars, light shafts and the dynamic resolution target. |
| Render distance | Chunks of full detail around the player. "preset" follows the preset. |
| Dynamic resolution | Lowers the render scale when the GPU cannot hold the target frame time and raises it again when there is headroom. "preset" follows the preset. |
| Render scale | The fixed scale used while dynamic resolution is off. |
| Field of view, Vertical sync | As named. |

Precedence is preset, then saved settings, then command line options. Benchmarks ignore the saved settings so results are comparable between machines.

## Performance testing

The engine measures itself, so a report from any machine can be compared with any other.

### Quick run

```
./build/dfe --benchmark
```

This flies a fixed path over seed 20240607 for 20 seconds (`--bench-seconds N` to change it), with vertical sync off, then prints a report. It never reads or writes a save.

### The full matrix

```
tools/perf_matrix.py --presets low,medium,high --sizes 1280x720,1920x1080 --seconds 10 --runs 3
```

Run it from the repository root with a Release build. The whole matrix runs inside one launch of the engine (`--bench-matrix`): between cases the engine switches preset and window size, puts the camera back at the start of the path, resets the day cycle, waits for the world around the start to be built, and discards the first second before recording. It prints one line per case as it goes, then one table, keeping the run with the median average frame rate. Results are saved as JSON in `perf_results/`. The example above is 18 cases and takes about four minutes; `--seconds 5` halves that and is enough on a fast machine. To compare a change with an earlier run:

```
tools/perf_matrix.py --presets low --sizes 1280x720 --baseline perf_results/<earlier>.json
```

Because the process stays warm, the world and the driver's shader cache are already loaded after the first case. The matrix therefore measures steady-state rendering, which is what preset, size and shader changes affect. It understates streaming cost and says nothing about cold start. For those, add `--isolated`, which launches the engine for every run as the earlier versions of the tool did (about ten times slower), or use a single `--benchmark`.

Some window managers clamp or ignore a resize request; tiling managers do. The table shows the size that was actually rendered. Close other applications first, and leave the machine plugged in with its power profile set to performance. The first case includes driver warm-up, so run at least `--runs 2` when comparing close results.

### Single run output options

| Option | Effect |
|--------|--------|
| `--bench-json FILE` | Write the summary as JSON. |
| `--bench-csv FILE` | Write one row per frame: total, CPU, stream, render submit, swap, GPU, per GPU section, render scale, draw calls, vertices, uploads. |
| `--bench-matrix SPEC`, `--bench-runs N` | Run `preset:WxH,preset:WxH` cases in one launch, repeated N times run after run. `--bench-seconds` then applies per case and `--bench-json` receives an array. |
| `--bench-label TEXT` | Free text copied into the JSON, for example the machine name. |
| `--preset NAME`, `--render-distance N`, `--width N`, `--height N` | The configuration under test. |
| `--render-scale S`, `--dynamic-res` | Fixed render scale from 0.4 to 1, or the dynamic controller. |
| `--no-render` | CPU side only: generation, light, meshing and streaming without a window. |

### Reading the report

```
fps avg: 61.2   1% low: 47.9
frame ms  avg 16.34  p50 15.90  p95 19.80  p99 22.40  max 31.20   hitches 1
cpu ms    avg 7.10  (stream 1.90, render submit 5.10)   swap wait avg 9.20
gpu ms    avg 13.80  (opaque 8.1, cutout 1.2, sky 0.4, water 1.1, rain 0.0, ui 0.3, post 0.6, entity 0.0)
```

| Line | How to read it |
|------|----------------|
| fps avg, 1% low | Average frame rate, and the frame rate of the slowest 1 percent of frames. The 1% low is what a player feels as stutter; a high average with a low 1% low means spikes. |
| frame ms p50, p95, p99, max | The distribution. A p99 far above p50 points at streaming or meshing spikes rather than steady load. |
| hitches | Frames longer than 25 ms. The worst eight are listed with their stream, render and swap times and the number of chunk uploads. A hitch with many uploads is a streaming cost; one with few is something else. |
| cpu ms | Time the main thread spent working. `stream` is world streaming and job completion, `render submit` is issuing draw calls. |
| swap wait | Time waiting for the display or the GPU after submission. Large values mean the GPU is the limit, or vertical sync is on. |
| gpu ms | Measured with timer queries, split by pass. This is the number that identifies the limit. It reads "unavailable" on drivers without timer queries; the dynamic resolution controller then falls back to swap wait. |
| render scale avg, min | Below 1.00 the dynamic controller reduced resolution to hold the target. |
| peak memory, cold start | Working set, and seconds from launch to a playable world. |

Deciding what is limiting a frame:

* GPU total close to the frame time: GPU bound. Look at the section that dominates. Lower the render scale or render distance, or report the section.
* CPU total close to the frame time with a small GPU total: CPU bound. A large `render submit` means draw submission, a large `stream` means world generation or meshing on the main thread.
* Both well below the frame time while the frame rate is capped: vertical sync or a driver frame cap is active. Benchmarks turn vertical sync off, so a cap there comes from the display driver.
* Spikes only in `stream` with many uploads: chunk upload bursts. Report the hitch lines.

### Budgets

The report ends with a pass or FAIL line for each budget on the reference machine (Low preset, 720p, render distance 8): 60 fps average, 1% low above 40, no frame above 25 ms, memory below 1.5 GB, cold start under 10 seconds. A result from a faster machine passing these says little; the budgets are for the reference machine. The numbers that matter on any machine are the GPU section times and the comparison with a baseline of the same machine.

### What to send back

For a useful report, attach the table from `tools/perf_matrix.py`, the `gl:` line (it names the GPU and driver), and for any FAIL the CSV of that run (`--bench-csv`). Software rendering (llvmpipe) numbers are only useful for checking that the tooling runs.

## Development workflow

`--dev` reloads shaders, the atmosphere file, presets and entity types when their files change, and F5 or the `reload` console command does it on demand. A reload that has an error keeps the previous version running and prints the problem. Blocks, textures and scripts are not reloaded, because block ids are stored in every loaded chunk; restart the game to change them.

## Modding

See [docs/MODDING.md](docs/MODDING.md) for the complete guide and API reference. Example mods are in `examples/mods`.
