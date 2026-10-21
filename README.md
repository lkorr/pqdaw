# pqdaw

A performance-first C++ DAW modelled on Ableton Live's Arrangement View, with:

- a non-MIDI pitch system (any EDO, morphable MOS scales, Scala tunings, adaptive just intonation with virtual fundamentals and proportional JI objects),
- a circuit-diagram signal language under every device, where any output can modulate any parameter at audio rate,
- a JIT-compiled engine (LLVM) designed so that no CPU cycle is spent on anything the patch doesn't need.

Status: early construction. See [SPEC.md](SPEC.md) for the full specification, [docs/TASKS.md](docs/TASKS.md) for the build plan, and [docs/PROGRESS.md](docs/PROGRESS.md) for the log.

Windows 10/11 x64 only for now.

## Building

Requirements: Visual Studio 2022 (MSVC v143), CMake ≥ 3.28, Ninja, Vulkan SDK, LLVM 23.1.3 prebuilt (`clang+llvm-23.1.3-x86_64-pc-windows-msvc`) extracted to `C:\dev\llvm-23.1.3` (or set `PQ_LLVM_DIR`).

```powershell
./scripts/build.ps1            # configure + build (dev preset) + run tests
./scripts/build.ps1 -Preset release
```

Personal, non-commercial project.
