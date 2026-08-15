# CacheScope

A portable CPU cache and memory-hierarchy characterization suite in C++.

CacheScope measures what a machine's memory system actually does including access
latency at every working-set size, streaming bandwidth, cache line size,
associativity, TLB reach, and the cost of cache-line contention between cores. Results
are written into a self-contained HTML report plus CSV, JSON and Markdown. Point the
comparison tool at reports from several machines and it overlays their curves.

It is designed to run on anything: old and new CPUs, x86 and ARM, 32-bit and
64-bit, Windows, Linux and macOS. The headless benchmark has **no third-party
dependencies at all** meaning that CMake and a C++ compiler are enough.

---

## Quick start

One command builds, tests, benchmarks and opens the report:

```bash
bash scripts/run_experiment.sh
```

On Windows:

```bash
powershell -ExecutionPolicy Bypass -File .\scripts\run_experiment.ps1
```

If you prefer to drive CMake yourself:

```bash
cmake --preset headless-release
```

```bash
cmake --build --preset headless-release --parallel
```

```bash
ctest --preset headless-release
```

```bash
./build/headless-release/bin/cachescope_cli --preset standard --out results
```

On Windows the binary is `build\headless-release\bin\cachescope_cli.exe`. Every
generator puts executables in the same `bin` directory, so the path above works
on all platforms.

A run produces:

```text
results/
├── cachescope_2026_08_15_...html   self-contained: charts, tables, caveats
├── cachescope_2026_08_15_....csv   for the comparison tool and spreadsheets
├── cachescope_2026_08_15_....json  full statistics for your own analysis
└── cachescope_2026_08_15_....md    paste-ready summary for a write-up
```

Runtime is roughly 20 s for `quick`, 90 s for `standard` and 4–6 minutes for
`deep`, depending on cache sizes. Progress is shown while it works.

---

## Requirements

| | Minimum | Notes |
|---|---|---|
| CMake | 3.21 (3.22 to use `--preset`) | Presets are convenience only; plain `cmake -S . -B build` works |
| Compiler | GCC 11+, Clang 14+, MSVC 19.30+ (VS 2022) | C++23 is used when available and C++20 otherwise |
| Dependencies | none | vcpkg, SDL3 and Dear ImGui are needed **only** for the optional GUI |

**Do not benchmark a Debug build.** CMake defaults to Release here for exactly
that reason, and any report produced without `NDEBUG` carries a warning saying
its numbers are meaningless.

---

## What it measures

Each experiment answers a specific question, and the report keeps what the
hardware *reports* strictly separate from what CacheScope *observed*.

| Experiment | Question | Method |
|---|---|---|
| Dependent latency | How long does one access take at each working-set size? | Random pointer chase, one node per cache line. Each address comes from the previous load, so the CPU cannot overlap misses. |
| Read / write / memcpy | What streaming bandwidth does one core get? | Sequential kernels that *may* vectorize and prefetch, because that is what real code does. |
| Line-pair probe | How big is a cache line, measured rather than reported? | A second load whose address depends on the first, at increasing offsets. Detected as a change point in the curve. |
| Stride probe | How far does spatial locality and prefetching reach? | Fixed buffer, stride from 4 B to 2 KiB. |
| Conflict-miss probe | How associative is each cache? | N blocks spaced by a power-of-two stride all map to one set. Latency jumps once N exceeds the way count. |
| TLB reach probe | How many pages can be mapped before translation costs bite? | One cache line touched per page, so the data stays small while the number of translations grows. |
| Coherency probe | What does sharing a cache line between cores cost? | Two threads doing identical atomic increments, on the same line and on separate lines. |

Reported per measurement: median, mean, min/max, p05/p95, MAD and coefficient of
variation, so you can see how stable a number is rather than trusting a single
sample.

### About the numbers

CacheScope never claims a measurement isolates one cache level. Real CPUs have
sliced and non-inclusive caches, adjacent-line and stride prefetchers, hybrid
core types, NUMA, cache hashing, SMT contention, frequency scaling and
virtualization. The report therefore separates:

- **hardware-reported geometry**: "the OS says this CPU has a 24 MiB L3 with
  64-byte lines", sourced from the OS, or from CPUID when the OS says nothing;
- **experimental observations**: "dependent latency reached 8x its floor at a
  2 MiB working set, and the conflict knee at a 4 KiB stride was 12 blocks".

Where an experiment cannot decide, it says "not decisive" instead of guessing.
On a machine with an adjacent-line prefetcher, for example, the line probe
legitimately reports either the 64-byte line or the 128-byte pair the hardware
actually moves, and the report says so.

Nanosecond values are measured directly. Cycle counts are derived from the core
frequency the OS reports, which some systems report as a static nominal value;
when CacheScope detects that, it says the cycle counts are a lower bound.

---

## Comparing machines

Collect the CSV reports and run:

```bash
./build/headless-release/bin/cachescope_compare --out comparison.html old_laptop.csv new_laptop.csv desktop.csv
```

A directory works too, which is easier on Windows where the shell does not
expand globs:

```bash
./build/headless-release/bin/cachescope_compare --out comparison.html results
```

The output has overlaid curves for every probe plus a cache-level table that
compares each machine at a working set near half of *its own* reported cache
size, so machines with different cache sizes are still compared at a comparable
point in their own hierarchy.

### Measurement protocol

For comparisons you intend to defend:

1. Same CacheScope version, same preset, on every machine.
2. Idle physical machine, stable power and thermal state, on AC power.
3. Three runs per machine: `--repeat 3` also emits a run-to-run comparison.
   If those three curves do not overlap, the machine is too noisy to compare
   yet due to the environment, not the hardware.
4. On hybrid CPUs (Intel P/E cores, ARM big.LITTLE, Apple Silicon), pin to the
   same core class with `--cpu N`. CacheScope warns when it detects more than
   one geometry per level.
5. Note anything unusual in the report's Warnings section before drawing
   conclusions.

---

## Command-line reference

```text
Run selection
  --preset quick|standard|deep  Accuracy/runtime preset (default: standard)
  --label NAME                  Machine label used in reports and comparisons
  --out DIR                     Report directory (default: results)
  --repeat N                    Run N times and also emit a run-to-run comparison
  --formats LIST                html,csv,json,md,all (default: all)

Sweep control
  --min-kib N                   Smallest working set (default: 4)
  --max-mib N                   Largest working set (default: automatic)
  --samples N                   Samples per point (minimum 3)
  --sample-ms N                 Target duration of each timing sample
  --cpu N                       Prefer logical CPU N
  --no-pin                      Do not pin the benchmark thread

Experiment selection
  --no-copy --no-line --no-stride --no-assoc --no-tlb --no-coherency
  --only-sweeps                 Only latency and throughput sweeps

Output
  --quiet                       No progress output
  --open                        Open the HTML report when finished
  --info                        Print detected hardware and exit
  -h, --help                    Show help
  --version                     Print version
```

---

## Optional GUI

The GUI is a thin shell around the same engine: inspect hardware, start a run,
watch live progress, cancel it, and export the same reports. It needs vcpkg,
which supplies Dear ImGui and SDL3.

```bash
cmake --preset release
```

```bash
cmake --build --preset release --parallel
```

```bash
./build/release/bin/cachescope_gui
```

The `release` preset requires `VCPKG_ROOT` to point at a vcpkg checkout:

```bash
export VCPKG_ROOT=/path/to/vcpkg
```

On Windows PowerShell:

```bash
$env:VCPKG_ROOT = "C:\dev\vcpkg"
```

On Linux, SDL3 needs system development packages; the `gui` job in
[.github/workflows/ci.yml](.github/workflows/ci.yml) lists the exact `apt`
packages.

---

## Building for 32-bit

32-bit builds are supported and exercised in CI. The automatic working-set
ceiling is capped more conservatively there, because a 32-bit process cannot
address a large sweep.

Windows:

```bash
cmake --preset windows-x86
```

Linux (needs `g++-multilib`):

```bash
cmake --preset linux-x86
```

---

## Build options

| Option | Default | Effect |
|---|---|---|
| `CACHESCOPE_BUILD_GUI` | `OFF` | Build the Dear ImGui GUI. Requires vcpkg. |
| `CACHESCOPE_BUILD_TESTS` | `ON` | Build the test binary and register CTest tests. |
| `CACHESCOPE_WERROR` | `OFF` | Treat compiler warnings as errors. |

Available presets: `headless-release`, `headless-debug`, `release` (GUI),
`ninja-release`, `windows-x86`, `linux-x86`.

---

## Troubleshooting

**`No CMAKE_CXX_COMPILER could be found`, or the Ninja generator fails on
Windows.** Use `--preset headless-release`, which uses the platform's default
generator; on Windows that is Visual Studio, which locates MSVC itself and does
not need a Developer Command Prompt. The `ninja-release` preset is the one that
requires `ninja` on PATH and, on Windows, a developer shell.

**`Could not find a package configuration file provided by "imgui"`.** You
configured with the GUI enabled but without vcpkg. Either set `VCPKG_ROOT` and
use `--preset release`, or build headless with `-DCACHESCOPE_BUILD_GUI=OFF`
(the default).

**Results vary a lot between runs.** Check the report's warnings and the
coefficient of variation column. Common causes: another process is busy, a
laptop on battery, thermal throttling, or a virtual machine. Run `--repeat 3`
and compare before trusting any single number.

**The cycle counts look too low.** Some systems report a static nominal core
frequency rather than the boosted clock. CacheScope detects this and adds a
warning; the nanosecond values are unaffected.

---

## Project layout

```text
include/cachescope/   public headers: benchmark.hpp, compare.hpp
src/benchmark.cpp     measurement engine: kernels, timing harness, experiments
src/platform.cpp      OS/CPUID introspection, affinity, core frequency
src/report.cpp        HTML, CSV, JSON and Markdown report writers
src/compare.cpp       CSV reader and comparison-report writer
src/main.cpp          cachescope_cli
src/compare_main.cpp  cachescope_compare
src/gui_main.cpp      cachescope_gui (optional)
tests/test_main.cpp   dependency-free test runner
docs/EXPERIMENTS.md   what each experiment shows, with suggested exercises
```

The measurement engine is deliberately separate from every front end: the CLI,
the GUI and the tests all call the same library, so the GUI cannot drift from
the numbers the CLI produces.

## Further reading

[docs/EXPERIMENTS.md](docs/EXPERIMENTS.md) explains each experiment in operating
systems terms including why a curve bends where it does, what it tells you about virtual
memory, scheduling and concurrency, and a set of exercises that use CacheScope
to demonstrate them.
