# KINETIC

A professional editor built specifically for anime music videos (AMVs). This repository holds the **engine**: a UI-agnostic C++20 core covering the timeline, velocity, keyframes, effects, beat sync, rendering and export. The Qt 6 desktop shell is built on top of it in the next milestone.

> Status: **Milestone 1 (engine core) is complete and tested on Linux.** **Milestone 1b (Qt 6 desktop app + Windows support) is written but has not been compiled yet**: the build machine had no Qt and no Windows toolchain. Expect a few compile fixes on first build. Nothing simulates functionality: every UI control calls the same engine API the tests exercise.

Read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) first. It covers the risk evaluation, the stack comparison (C++/Qt 6/RHI/FFmpeg vs Rust/wgpu vs web), every data model, the render/decode/export pipelines, the roadmap and the testing strategy.

## Build

Requirements:
- A C++20 compiler (GCC 11+, Clang 14+, MSVC 2022)
- CMake 3.20+
- `ffmpeg` and `ffprobe` on `PATH`, or pointed to with `KINETIC_FFMPEG` / `KINETIC_FFPROBE`

There are no other dependencies.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/kinetic_tests              # 52 tests: unit + real FFmpeg integration
```

Sanitizer build: `-DKINETIC_SANITIZE=ON` (ASan + UBSan).

## Build the desktop app (Windows)

1. Install **Visual Studio 2022** (Desktop C++ workload) and **CMake 3.20+**.
2. Install **Qt 6.5+** for MSVC 2022 x64 with the **Qt Multimedia** module (Multimedia is optional, but without it there is no audio during preview).
3. Install **FFmpeg** (for example the gyan.dev "full" build) and add its `bin` folder to `PATH`, so `ffmpeg -version` works in a new terminal.
4. From a *x64 Native Tools* prompt in this folder:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -DKINETIC_BUILD_UI=ON -DCMAKE_PREFIX_PATH=C:\Qt\6.7.2\msvc2019_64
cmake --build build --config Release
C:\Qt\6.7.2\msvc2019_64\bin\windeployqt.exe build\Release\kinetic.exe
build\Release\kinetic_tests.exe
build\Release\kinetic.exe
```

macOS/Linux: the same with `-DKINETIC_BUILD_UI=ON -DCMAKE_PREFIX_PATH=<qt>/lib/cmake`.

### What the desktop app does
- **Welcome screen**: new project (1920×1080 at 60 fps by default; 16:9 / 9:16 / 1:1 / 4:5 presets), open, recent projects, import media, and a generated tutorial project.
- **Layout**: dark studio theme (spec palette) and dockable panels. Workspaces: Standard Edit, Velocity Studio, Effects Studio and Audio Studio. Each workspace remembers your own layout, and there's a reset option.
- **Media panel**: drag files onto the window to import. Shows thumbnails, specs and missing-media flags. Drag media onto a track to add it.
- **Timeline**:
  - ruler, beat and drop markers
  - clips with speed overlays, fx/shake badges and audio waveforms
  - drag to move (including across tracks), drag edges to trim, snapping to edits, markers and the playhead
  - per-track hide/mute, solo and lock toggles; Ctrl+wheel zoom
- **Viewer**: renders through the engine at Full, Half or Quarter resolution and shows render time and render warnings. During playback, audio is the master clock and slow frames are skipped rather than slowed down.
- **Inspector**:
  - transform, blend, fit, punch-in
  - velocity presets, intensity and interpolation (optical flow is listed but refuses, because it isn't installed)
  - camera shake
  - effect stack: add, reorder, duplicate, remove, on/off, intensity, Quick or Advanced parameters
- **Velocity Studio graph**: click to add a key, drag to move it (Shift snaps to 0.25×), right-click to delete, double-click to cycle easing.
- **Beat Sync menu**: detect beats in the music track (review dialog, then markers plus drop markers), cut the selected clip at beats or strong beats, white flashes on strong beats, shake on beats.
- **Editing and saving**: undo/redo for everything, split (S), delete or ripple delete, duplicate, adjustment layers.
- **Safety**: autosave every 2 minutes, crash recovery on open, unsaved-changes prompt.
- **Export**: runs in the background with progress, ETA and cancel. MP4/MOV/WebM/MKV/WAV/PNG sequence; quality presets.

### Shortcuts
Space play/pause · ←/→ frame step · Home start · S split · Del delete · Shift+Del ripple delete · Ctrl+D duplicate · B detect beats · Ctrl+Z / Ctrl+Shift+Z undo/redo · Ctrl+I import · Ctrl+M export · Ctrl+S save · Ctrl+= / Ctrl+- zoom.

## Try it

```bash
# Generate footage + a synthesized track, build a full AMV project through the engine API, render it
./build/kinetic-cli demo ./demo --seconds 12 --size 1280x720

./build/kinetic-cli beats song.mp3 --json beats.json      # tempo, beats, downbeats, onsets, sections, drop
./build/kinetic-cli render ./demo/demo.kinetic out.mp4 --quality high
./build/kinetic-cli render ./demo/demo.kinetic out.webm --range 150:240
./build/kinetic-cli frame  ./demo/demo.kinetic 169 drop.png --scale 0.5
./build/kinetic-cli effects                               # registry with every parameter
./build/kinetic-cli validate ./demo/demo.kinetic
./build/kinetic-cli recover  ./demo/demo.kinetic          # is there a valid autosave newer than the project?
```

The demo does the following, all through undoable commands:
- imports 4 H.264 clips and a music track
- detects beats, places beat markers and lays out clips with the beat-distribution planner
- adds velocity ramps (Anime Velocity, Impact Ramp, Smooth Slowdown, with frame blending)
- adds camera shake, glow pulses and RGB-split pulses keyed to beats
- adds a nested "Impact" composition at the detected drop: punch-in, directional blur, speed lines, white flash
- adds a build-up adjustment layer (zoom blur and speed lines), white flashes on strong beats, and a global grade (colour, vignette, grain)

It saves a `.kinetic` project and exports MP4 with audio.

## Milestone 1 report

### Implemented and tested

| Area | What works | Tests |
|---|---|---|
| Project model | Compositions, tracks (video/audio; hide/lock/mute/solo), clips (media, nested comp, solid, adjustment), markers, validation (overlaps, cycles, ids) | `test_timeline` |
| Undo/redo | Snapshot commands, macros, drag merging, exact state restore (byte-compared) | `timeline_undo_redo_restores_exact_state` |
| Editing | Add, delete, ripple delete, move (incl. across tracks), trim start/end, roll, slip, split, duplicate, snapping (clip edges, markers, playhead), locked-track protection, source-bounds checks | `test_timeline` |
| Keyframes | Hold/linear/cubic-Bézier (CSS-exact), 8 ease presets incl. overshoot, time scaling, exact integrals | `test_core` |
| Velocity | Speed curve → source time (integrated), reverse, freeze, 11 editable presets, split continuity, frame blending; optical flow refuses to render instead of degrading silently | `test_core`, `test_render`, `test_integration` |
| Beat sync | Mel-band onset detection with kick/snare/hat labels, tempo, DP beat tracking, downbeats, sections, drop detection; review-before-apply plans for markers, cuts at (strong) beats, distribution to beats, flashes, effect pulses, shake | `test_audio`, `test_timeline` |
| Project files | Versioned JSON, schema migration (v1→v2), refuses newer schemas, atomic save + fsync, rotating backups, autosave sidecar, recovery that never offers a corrupt autosave, missing-media detection | `test_io` |
| Presets | Versioned effect-stack / velocity / shake presets; missing effects reported and preserved, never dropped | `test_io` |
| Media | ffprobe import (NTSC rates exact), frame-accurate decoding with random access, decode-at-display-size, byte-budget LRU frame cache | `test_integration` |
| Rendering | Linear-light premultiplied compositor, 10 blend modes, affine transforms, fit/fill, nested comps, adjustment layers, solo/hidden, camera shake, preview scale that matches the export | `test_render` |
| Effects | 13 effects: Gaussian, directional and zoom blur; glow; RGB split; impact flash; colour adjust; vignette; film grain; posterize; sharpen; scanlines; anime speed lines. Quick/Advanced param flags, per-effect mix, bypass, deterministic output | `test_render` |
| Export | MP4/MOV/WebM/MKV/WAV/PNG sequences, quality presets (encoder-only, never effect downgrades), velocity-aware audio mixdown, progress/ETA, cancel without leaving partial files | `test_integration` |

### Measured
- Test suite: 52/52 passing.
- Demo render, 1280×720 at 30 fps, 360 frames, with full effect stacks on most frames: about 5 frames/s on the 2-vCPU build container, using the CPU reference renderer. This is a baseline for the GPU backend, not a product claim.

### Not implemented yet (by design, see roadmap)
- The desktop app has not been compiled or run yet (see Status). Not yet in the UI: keyframe graph editor for non-velocity properties, configurable shortcuts, interactive onboarding tour, direct manipulation handles in the viewer.
- **GPU backend** (Qt RHI). The CPU path is the reference that GPU shaders must match.
- Masks and rotoscoping, track mattes, text/typography, a transition library (flashes and impact comps work; a dedicated transition object does not), colour wheels, curves and LUTs, audio-reactive parameter bindings (beat-keyed pulses exist), proxies, hardware decode, VFR conforming, mixing audio embedded in video clips, motion tracking, optical-flow slow motion, node compositing, expressions, plugins.

### Known issues
- Windows support (Win32 subprocess pipes, durable atomic saves via `MoveFileEx`) is written but untested; Linux paths are tested.
- Preview renders on the UI thread; heavy stacks will lower the preview frame rate (use Half/Quarter). A background render thread with look-ahead is the next optimisation.
- The media backend runs FFmpeg as a subprocess, which costs one extra copy per frame. The libav in-process backend sits behind the same `IDecoder` seam.
- Blend modes are computed in linear light, so Screen/Overlay won't exactly match After Effects' gamma-space defaults. A gamma-space option is planned.
- Shipping builds must not link GPL encoders (x264). See risk #5 in the architecture doc.

## Layout

```
src/core      JSON, rational time
src/anim      keyframe engine
src/project   data model, velocity engine
src/edit      undo stack, edit operations, beat-sync planner
src/io        serialization, migrations, crash-safe storage, presets
src/media     subprocess, probe, decoder, frame cache, audio decode
src/render    image/colour, effects registry, compositor, evaluator
src/audio     FFT, beat/onset/section analysis
src/export    exporter + audio mixdown
apps/kinetic-cli   CLI + demo builder
tests         unit + integration tests (no external framework)
docs          architecture
```
