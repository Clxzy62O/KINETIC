# KINETIC — Architecture Evaluation & System Design

Status: living document. Revision 1 (Milestone 0/1). Every decision here is a proposal backed by reasoning; items marked **[measured]** have a test or benchmark in the repo, items marked **[planned]** do not exist yet.

---

## 1. Concept evaluation

### 1.1 What makes this hard (ranked by risk)

| # | Risk | Why it matters for AMVs specifically | Mitigation |
|---|------|--------------------------------------|------------|
| 1 | **Long-GOP random access** | Anime sources are almost always H.264/HEVC (often 10-bit HEVC in MKV). AMV editing is scrub-heavy and cut-heavy (a 3-minute AMV can have 200+ cuts). Seeking a long-GOP stream means decoding from the previous keyframe — at 10-bit 1080p that can be 100–500 ms per seek. | Intra-only proxies (generated in the background), a decoder pool with per-clip sequential readers, a decoded-frame cache, and GOP-aware prefetch around the playhead. |
| 2 | **Preview ≡ export determinism** | The spec forbids silent effect downgrades. GPU shaders and CPU code differ in precision, sampling and blending order. | One effect definition, two backends; the CPU backend is the *reference*. Every GPU shader has golden-image tests against the CPU reference with a defined tolerance (ΔE / max abs error). Quality modes change *resolution*, never *algorithm*, unless the user picks it and the UI says so. |
| 3 | **Optical-flow slow motion** | AMV "velocity" edits rely on smooth slow-mo. Anime is drawn on 2s/3s with flat colours and hard lines: classical optical flow fails badly (no texture, duplicated frames). | Three explicitly labelled modes: Frame Sampling (nearest), Frame Blending, Optical Flow. Optical flow ships as an optional, downloadable ML module (RIFE-class model via ONNX Runtime) so the base install stays small. Duplicate-frame detection runs first so held drawings aren't interpolated into mush. |
| 4 | **Rotoscoping / character isolation** | Hard problem in general; segmentation models are large (100 MB–2 GB). | Manual Bezier masks + tracking are core (Phase 3/5). AI segmentation is an optional local module; results are always converted to editable Bezier masks. Never required, never cloud-only. |
| 5 | **Licensing of the media stack** | FFmpeg is LGPL only if built without GPL parts. **x264/x265 are GPL** — linking them makes KINETIC GPL. Qt is LGPL v3 (dynamic linking required) or commercial. | Ship an LGPL FFmpeg build. H.264/HEVC encode via OS hardware encoders (NVENC/QSV/AMF/VideoToolbox/Media Foundation) plus a permissive software fallback (OpenH264 BSD, SVT-AV1 BSD, libvpx BSD). Offer x264 as a user-installed optional plugin, or license it commercially. Decide before shipping 1.0. |
| 6 | **Source quirks** | Fansub/BD rips: telecined 29.97 (3:2 pulldown), VFR MKVs, mis-tagged BT.601/709, limited vs full range, embedded ASS subtitles and fonts. | Probe and normalise at import: IVTC option, constant-frame-rate conforming, explicit colour-tag override in the media inspector, subtitle stream detection. |
| 7 | **Memory** | 1080p RGBA float = 33 MB/frame; 4K = 133 MB. A cache of 100 frames at 4K float is 13 GB. | Cache 8-bit or half-float per stage; budgets by bytes, not frames; LRU with cost-aware eviction; preview at ½ or ¼ resolution by default for heavy comps. |
| 8 | **Audio/video sync** | Beat-synced cuts must land frame-exactly. Audio clocks and video frame clocks drift with 29.97/23.976 rates. | Rational time everywhere in the model; audio is master clock in playback; beat times are stored in seconds and snapped to composition frames only at the edit. |
| 9 | **Undo/redo across a large model** | Interactive drags generate hundreds of mutations; nested comps and presets touch many objects. | Command pattern with explicit before/after state per entity, macro (compound) commands, merge-on-drag. **[measured]** |
| 10 | **Crash safety** | Spec: never overwrite a good project with an incomplete file. | Write-to-temp + fsync + atomic rename; rotating backups; autosave to a sidecar file; recovery validates before offering. **[measured]** |

### 1.2 Build first / delay / custom engineering

**Build first** (everything else depends on them): rational time, the project/timeline model, the undo stack, project persistence, media probe/decode, a deterministic compositor, export. Beat detection and the velocity engine come immediately after, because they *are* the product's identity.

**Delay**: node compositing, expressions, custom shaders/plugins, AI tools, motion tracking, rotoscoping, optical flow. All of them need a stable core and stable data formats first.

**Custom engineering required** (no off-the-shelf drop-in): the velocity engine (time-remap integration with frame blending), the AMV impact-effect library, the beat-sync edit planner, the timeline widget (a stock Qt widget won't hit the interaction targets), the cache/invalidation system, and the GPU effect framework with CPU reference parity.

**Off-the-shelf**: FFmpeg (decode/encode/mux), Qt 6 (UI, RHI GPU abstraction), optionally OpenColorIO (LUTs/colour management), ONNX Runtime (optional AI modules), FreeType + HarfBuzz (text; Qt already bundles both).

---

## 2. Technology stack — options compared

| Criterion | **A. C++20 + Qt 6 + Qt RHI + FFmpeg (recommended)** | B. Rust + wgpu + Slint/egui + ffmpeg-next | C. Tauri/Electron + WebGPU + WebCodecs |
|---|---|---|---|
| Desktop UI maturity (docking, high-DPI, accessibility, IME, shortcuts) | Excellent; QtWidgets docking + custom-painted timeline | Fair; egui is immediate-mode (weak accessibility), Slint is younger | Good UI, but web layout fights a dense pro timeline |
| GPU abstraction | Qt RHI → Vulkan / D3D11 / D3D12 / Metal / OpenGL from one code path | wgpu → Vulkan / D3D12 / Metal; excellent | WebGPU; sandboxed, limited texture-interop with decoders |
| Hardware decode → GPU texture zero-copy | Possible (D3D11VA/VAAPI/VideoToolbox interop) | Possible but more FFI work | Hard; WebCodecs codec coverage is browser-dependent |
| FFmpeg integration | Native C API | Bindings, mostly fine | Requires a sidecar process or WASM build |
| Ecosystem (OpenTimelineIO, OCIO, OpenFX, ONNX Runtime) | All native C++ | Bindings of varying quality | Poor |
| Memory safety | Manual discipline + sanitizers | Strong | Strong (JS), but huge runtime |
| Install size | ~60–120 MB Qt subset + ~30 MB FFmpeg LGPL | Smaller (~40–80 MB total) | Electron 150 MB+ before features |
| Spec fit ("not a web app pretending") | ✔ | ✔ | ✘ (rejected) |

**Recommendation: A.** The deciding factors are Qt RHI (one renderer for every OS backend, which the spec asks for), first-class FFmpeg and colour-pipeline interop, and the maturity of QtWidgets for a dense, keyboard-driven pro UI. Option B is a credible second choice; it wins on memory safety and binary size. To keep that door open, the engine in this repo is **UI-agnostic C++ with no Qt dependency**, so it could be wrapped over a C ABI later. Option C is rejected on the spec's own terms.

Size target **[planned, to be measured]**: installed footprint ≤ 300 MB without optional AI modules; cold start to Welcome screen ≤ 2 s on a mid-range SSD laptop. No claim of beating After Effects is made until measured on matched projects (see §13).

---

## 3. System architecture

```
┌──────────────────────────────── Application (Qt 6) [planned] ─────────────────────────────────┐
│ Workspaces · Panels · Timeline widget · Graph editors · Viewer (RHI) · Inspector · Browser    │
└──────────────┬───────────────────────────────┬─────────────────────────────────┬──────────────┘
               │ Commands (undoable)           │ Queries / observers             │ Viewer frames
┌──────────────▼──────────────┐  ┌─────────────▼─────────────┐  ┌────────────────▼─────────────┐
│ edit/  CommandStack, edit   │  │ project/ Model (pure data)│  │ render/  Evaluator           │
│ operations, beat-sync       │──▶ Composition, Track, Clip, │◀─│ (comp tree @ time t) →       │
│ planner, snapping           │  │ Effect, Marker, Keyframes │  │ Compositor → Backend(CPU|GPU)│
└─────────────────────────────┘  └─────────────▲─────────────┘  └───────▲──────────┬───────────┘
                                               │ JSON (versioned)       │ frames    │
                               ┌───────────────┴──────────┐   ┌────────┴────────┐ ┌▼───────────┐
                               │ io/ Serialize, Storage   │   │ media/ Probe,   │ │ export/    │
                               │ (atomic save, autosave,  │   │ Decoder pool,   │ │ Exporter → │
                               │ recovery, migrations)    │   │ FrameCache,     │ │ encoder    │
                               └──────────────────────────┘   │ Proxies[planned]│ └────────────┘
                                                              └─────────────────┘
                               ┌──────────────────────────┐
                               │ audio/ decode, waveform, │──▶ markers / audio-reactive bindings
                               │ onset+beat analysis      │
                               └──────────────────────────┘
```

Rules (from spec §27, enforced by the module layout):
- `project/` is pure data. It knows nothing about the UI, rendering, or files.
- All mutation of the model goes through `edit/` commands. That is what makes undo correct.
- `render/` reads the model and media and produces pixels. It never mutates the model.
- UI state (selection, zoom, panel layout) lives in the app layer, never in the project file.

### 3.1 Module breakdown (this repo)

| Module | Files | Responsibility | Status |
|---|---|---|---|
| core | `src/core/Json.*`, `Rational.h`, `Log.h` | JSON DOM/parser/writer (no dependency), rational time | implemented, tested |
| anim | `src/anim/Keyframes.*` | Unified keyframe engine: hold / linear / cubic-bezier, easing presets | implemented, tested |
| project | `src/project/Model.*` | Project → Compositions → Tracks → Clips; effects; markers; IDs | implemented, tested |
| velocity | `src/project/Velocity.*` | Speed curve → source time (numeric integration), presets, reverse/freeze | implemented, tested |
| edit | `src/edit/Commands.*`, `Edit.*`, `BeatSync.*` | Undo stack; move/trim/split/ripple; snapping; beat-cut planner (preview-then-apply) | implemented, tested |
| io | `src/io/Serialize.*`, `Storage.*`, `Preset.*` | Versioned JSON, migrations, atomic save, backups, autosave/recovery, presets | implemented, tested |
| media | `src/media/Probe.*`, `Decoder.*`, `FrameCache.*`, `AudioDecode.*` | FFmpeg probe, sequential-reader decoder pool, byte-budget LRU | implemented (FFmpeg subprocess backend), tested |
| render | `src/render/Image.*`, `Effects.*`, `Compositor.*`, `Evaluator.*` | Linear-light premultiplied compositing, blend modes, transforms, effect registry, nested comps, adjustment layers, camera shake | implemented (CPU reference backend), tested |
| audio | `src/audio/Fft.*`, `BeatDetect.*` | STFT, spectral-flux onsets, tempo, DP beat tracking, strong/weak, kick/snare banding, energy curve, waveform peaks | implemented, tested |
| export | `src/export/Exporter.*` | Render queue item → FFmpeg encoder; audio mixdown; progress/cancel | implemented, tested |
| app | `apps/kinetic-cli` | CLI: probe, beats, render, frame, demo | implemented |
| ui | `apps/kinetic` (Qt) | Desktop shell | **planned (needs Qt 6 toolchain)** |

### 3.2 Media backend note

The production media backend links libavformat/libavcodec directly. That enables hardware decode, zero-copy GPU upload and precise packet-level seeking. This workspace has the FFmpeg *executables* but not the development headers, so milestone 1 ships a **subprocess backend**: `ffmpeg … -f rawvideo -pix_fmt rgba pipe:1`. It does real decoding, but there is an extra memory copy per frame. Both backends sit behind the same `IDecoder` interface, so swapping them doesn't touch any caller. No fake or placeholder frames are produced: if decoding fails, the error propagates.

---

## 4. Project file structure (on disk)

```
MyEdit/
  MyEdit.kinetic              ← project JSON (atomic writes)
  MyEdit.kinetic.bak1..3      ← rotating backups of the last known-good saves
  MyEdit.kinetic.autosave     ← autosave sidecar; only offered for recovery if valid AND newer
  .kinetic-cache/             ← disposable: proxies/, thumbs/, waveforms/, analysis/ (keyed by media fingerprint)
```

Media is **referenced, never copied or modified**. Each reference stores a path plus a fingerprint (size + mtime + a hash of the first/last 64 KB [planned]) for relinking. "Collect Project" [planned] copies the referenced media, fonts (where the licence flags allow), presets and LUTs into a self-contained folder and rewrites the paths to be relative.

## 5. Timeline data model

```
Project { schemaVersion, settings, media[], compositions[], rootCompositionId, nextId }
Composition { id, name, width, height, fps: Rational, durationFrames, background, tracks[], markers[] }
Track { id, kind: video|audio, name, enabled(hidden), locked, muted, solo, color, clips[] (sorted, non-overlapping) }
Clip  { id, name, kind: media|composition|solid|adjustment,
        mediaId | compositionId | solidColor,
        start (comp frames), duration (comp frames), sourceIn (seconds),
        velocity: SpeedCurve, timeInterp: sample|blend,
        transform: {anchor, position, scale, rotation, opacity : AnimCurve each},
        shake: CameraShake, blendMode, effects: EffectInstance[], gain }
Marker { id, frame, kind: manual|beat|strongBeat|section, color, label, group }
```

Invariants (checked by `Model::validate()` and by tests):
- Clips on a track never overlap. Edits that would overlap are rejected or resolved explicitly (ripple / overwrite).
- Composition references are acyclic, so a nested comp can't contain itself.
- IDs are unique and monotonically allocated (`nextId`), which keeps project diffs stable.

Time is **integer composition frames** on the timeline (frame-accurate editing) and **seconds (double)** inside source media. Composition fps is a `Rational` (24000/1001 etc.), so 23.976/29.97/59.94 are exact.

## 6. Keyframe data model

```
AnimCurve { constant: double, keys: Key[] }       // empty keys ⇒ constant
Key { t (seconds, clip-local), v, interpOut: hold|linear|bezier,
      outHandle (x1,y1), inHandle (x2,y2) }         // normalized cubic-bezier of the segment
```
- Each segment *i → i+1* is a cubic Bézier easing in normalised (time, value) space: P0=(0,0), P1=key[i].outHandle, P2=key[i+1].inHandle, P3=(1,1). x is solved by Newton iteration with a bisection fallback (monotonic in x, since handles are clamped to x∈[0,1]).
- Presets: Linear, Ease In, Ease Out, Ease In-Out, Punch (fast-out), Snap, Hold. Graph editors manipulate exactly these handles.
- y-handles may exceed [0,1] → overshoot (elastic, punch-in).
- Multidimensional properties are separate curves per channel. This keeps one engine for everything: position, effects, masks, audio-reactive base values.

## 7. Velocity engine

The speed curve `s(t)` is an `AnimCurve` over clip-local *output* time (seconds). Source time is

  `src(t) = sourceIn + ∫₀ᵗ s(τ) dτ`

- Integrated with composite Simpson's rule (max step 1/960 s), split exactly at keyframe boundaries so hold segments stay exact. It's evaluated on demand and deterministic. A per-revision lookup table is planned for very long clips; it isn't needed yet at measured cost.
- `s < 0` plays in reverse, `s = 0` freezes (hold keys give clean freeze frames), and `s` ramps give speed ramps.
- Presets (Smooth Slowdown, Impact Ramp, Beat Drop Ramp, Hyper Velocity, …) are generators that write ordinary keys: starting points, not locked animations.
- Frame-time interpolation per clip: `Sample` (nearest source frame) or `Blend` (weighted mix of the two neighbouring source frames). `OpticalFlow` exists as an enum value but is **rejected at render time with an error** until the module ships. It is never silently replaced.
- The UI draws the speed-coloured clip overlay from the same curve (`Velocity::sampleForDisplay`).

## 8. Effect architecture

```
EffectDef { id ("kinetic.glow"), version, name, category, params: ParamDef[] , process: fn }
ParamDef  { id, name, kind: float|color|bool|enum, default, min, max, animatable, quickMode: bool }
EffectInstance { defId, defVersion, enabled, mix (intensity 0..1), params: map<id, AnimCurve> }
```
- **Registry**: effects self-register. Presets and projects reference effects by `id@version`. A missing effect is reported, never dropped. The instance is kept as an opaque "unavailable" placeholder that renders as pass-through *with a visible warning* and is preserved on save.
- **Quick vs Advanced**: `quickMode` flags which params show in Quick Mode. Advanced exposes all params, keyframes and ordering.
- **Stacking**: an ordered list per clip. Bypass per effect, bypass the whole stack, and a global `mix`. Grouping and saved chains are presets (§11).
- **Backends**: each EffectDef has a CPU reference `process(Image&, EvalContext)` today. GPU kernels [planned] register against the same id and must pass golden-image parity tests.
- Implemented now (13): Gaussian Blur (pyramid for large radii), Directional Blur, Zoom Blur, Glow (threshold/radius/intensity/multi-octave spread/tint), RGB Split, Impact Flash (any colour, so white or black), Color Adjust (exposure/contrast/brightness/saturation/temperature/tint), Vignette, Film Grain (seeded per frame), Posterize, Sharpen, Scanlines, Anime Speed Lines (procedural, seeded, flicker rate). Camera Shake is a procedural transform modifier (seeded quintic value-noise blended with a sine; amplitude curve, frequency, rotation, zoom, directionality, decay, seed).
- Pixel-size parameters are defined in *layer pixels* (like After Effects) and scaled by the buffer's pixels-per-layer-pixel, so a ½-resolution preview looks the same as the export. **[measured]**

## 9. Rendering pipeline

Per output frame `f` of composition `C`:

1. **Evaluate** (`Evaluator`): walk the tracks bottom→top and collect the active clips at `f`. For each clip compute local time, then source time via velocity, then evaluated transform/effect parameters.
2. **Fetch**: media clips → `FrameProvider` (cache → decoder). Composition clips → recursive render of the nested comp at the remapped time (with a cycle guard). Solids → fill.
3. **Layer processing** in layer space: effects in stack order.
4. **Transform** into comp space: anchor/scale/rotate/translate plus camera shake, bilinear resampling. Quality mode scales the whole comp, not individual effects.
5. **Composite**: premultiplied alpha, **linear light** (sRGB decoded by LUT on ingest, encoded on output), blend modes Normal/Add/Screen/Multiply/Overlay/Soft Light/Hard Light/Difference/Lighten/Darken (W3C separable formulas), layer opacity. Note: blend modes are evaluated in linear light, which makes Screen/Overlay slightly harsher than After Effects' default gamma-space blending. A per-project "blend in gamma space" switch is planned for editors who want AE-matching looks.
6. **Adjustment clips** apply their effect stack to the accumulated result below them.
7. Output → viewer (preview) or encoder (export). Same code path, same effects, same order. Preview may use a lower *resolution scale*; the scale is shown in the viewer.

Caching [partially implemented]: decoded source frames (byte-budget LRU, implemented). Planned: per-layer post-effect cache keyed by `(clipId, frame, hash(params at t), upstream revision)`, so the key changes automatically when anything upstream changes, which gives correct invalidation without manual dirty tracking.

## 9b. Audio analysis (beat sync)

`audio/BeatDetect` works on mono PCM:
1. STFT with a ~46 ms Hann window and 10 ms hop.
2. The onset envelope is spectral flux over **48 mel-spaced bands**. With raw FFT bins, the hundreds of high-frequency hat bins outvote the few kick bins.
3. Onsets come from adaptive-threshold peak picking. Each one is labelled kick/snare/hat by its dominant band.
4. Tempo comes from tempo-weighted autocorrelation with a harmonic comb and a log-Gaussian prior around 120 BPM, then parabolic refinement.
5. Beats come from Ellis-style dynamic-programming tracking. The tracking envelope adds low-band onsets so the grid locks to kicks rather than off-beat hats.
6. Downbeats are the bar phase with the most kick energy plus accents.
7. The energy curve and sections come from a smoothed RMS level.
8. A **drop** is the beat with the largest step in broadband plus low-band (kick and bass) level. Build-up risers and snare rolls are mid/high frequency, so they don't win.

Measured on synthetic tracks: tempo within ±1.5 BPM at 90/120/128/150 BPM, every beat within 15 ms (under one 60 fps frame), ≥90% of strong beats on true downbeats, and the drop on the first loud downbeat within 30 ms. Real music is harder. Every result is turned into **editable markers**, and every beat-sync edit is a reviewable plan (`edit/BeatSync`).

## 10. Media decoding pipeline

```
import → probe (streams, codec, fps, duration, colour tags, pix_fmt, audio) → MediaRef
       → background jobs [planned]: thumbnails, waveform peaks, beat analysis, proxy (intra-only, ½/¼)
playback/render → FrameProvider.get(mediaId, sourceFrame, scale)
   → FrameCache hit?  → return
   → DecoderPool: reuse a reader positioned just before the frame (sequential read), else (re)open at
     nearest position with accurate seek → read forward → frames inserted in cache
```
Readers are per (media, scale) and sequential, which suits playback. Random access reopens the reader. Proxies [planned] make random access cheap. Hardware decode lives in the libav backend [planned].

## 11. Preset format

```json
{ "format": "kinetic-preset", "schemaVersion": 1, "kind": "effectStack",
  "name": "Impact Pack", "category": "Impact",
  "requires": [{"effect": "kinetic.glow", "version": 1}, ...],
  "payload": { "effects": [ EffectInstance... ] } }
```
Kinds: effectStack, velocityCurve, cameraShake, colorGrade, textStyle [planned], composition [planned]. When applied, the preset is deep-copied into the target, so it stays fully editable. Missing `requires` produces a report listing each missing effect, and the user decides.

## 12. Undo/redo architecture

- `Command { do(Project&), undo(Project&), label, mergeKey }`. Primitives: `AddClip`, `RemoveClip`, `ReplaceClip(before, after)`, `AddMarker`, `RemoveMarker`, `SetCompositionProperty`, …
- `MacroCommand` groups primitives. Every edit tool (split, ripple delete, trim, beat-cut) is a macro, so one Ctrl+Z undoes one user action.
- Interactive drags push commands with the same `mergeKey` within a gesture. They collapse to one undo entry.
- Commands store full before/after snapshots of the touched entity, never deltas computed against "current" state. That makes undo correct even after redo/undo interleavings. **[measured: tests replay do/undo/redo sequences and compare serialized state byte-for-byte]**
- The stack tracks a "clean index" for the unsaved-changes indicator.

## 13. Export architecture

`ExportJob { compositionId, range, settings (codec, container, crf/bitrate, preset, pix_fmt, audio rate/bitrate, scale), quality }` → render frames in order through the *same* Evaluator → raw RGBA → encoder process (libav in production) → container. Audio: all audible audio clips mixed in float, resampled to 48 kHz, written to a temporary WAV, muxed. There's a progress callback with frame count and ETA, and cancellation. Render queue / pause / resume [planned]: pause is implemented as stop-and-resume-from-frame into image sequences, or by holding the pipe for direct encodes.

Encoders: hardware first (NVENC/QSV/AMF/VideoToolbox) when available, then a licence-compatible software fallback (see risk #5). This workspace's FFmpeg includes libx264, which is used for verification only.

## 14. GPU processing architecture [planned, Milestone 3]

- Qt RHI device, one per app. Textures RGBA16F for the compositing chain, RGBA8 for caches.
- Effects are compute or fragment passes compiled from GLSL to SPIR-V with QShaderBaker at build time (no runtime shader compiler shipped, which keeps the install small).
- A texture pool keyed by size/format avoids reallocations. Separable blurs and mip-chain glow (Kawase / dual-filter), with sigma matched to the CPU reference.
- Hardware decode surfaces imported directly where interop exists, CPU upload otherwise.
- Fallback: the CPU backend (this repo) for machines without a capable GPU, with reduced preview resolution.

## 15. Performance targets (to be benchmarked, not claimed)

| Metric | Target | Bench project |
|---|---|---|
| Cold start → welcome | ≤ 2 s | — |
| Timeline interaction (drag, trim) | ≤ 8 ms per UI frame at 500 clips | `bench/timeline_500` [planned] |
| Preview, 1080p60, 3 layers + glow + shake (GPU) | real-time at Full | `bench/impact_1080p` [planned] |
| Preview, same, CPU fallback | real-time at ¼ resolution | same |
| Undo/redo of any single action | ≤ 5 ms | unit bench |
| Peak RAM, 1080p 3-min AMV | ≤ 4 GB with default cache budget | — |

Current CPU reference numbers are printed by `kinetic-cli render` (frames/s). They're a baseline, not a product claim.

## 16. Development roadmap (re-sequenced)

| Milestone | Content | Exit criteria |
|---|---|---|
| **M1 Engine core** (this delivery) | Model, keyframes, velocity, undo, storage, FFmpeg decode, CPU compositor, 11 effects, beat detection, export, CLI | All tests green; CLI renders a project with velocity ramp + effects + beat-synced flashes to MP4 with audio |
| M1b Desktop shell | Qt 6 app: welcome screen, project setup (1080p60 default), media browser, viewer, timeline widget, inspector, playback with audio clock, autosave timer | Usable editor for cut/trim/split/export |
| M2 AMV editing | Velocity graph editor UI, keyframe graph UI, waveform + beat markers UI, shake presets, basic transitions (flash/zoom/whip/glitch), beat-sync review dialog | A recognisable AMV can be made end-to-end in the GUI |
| M3 GPU + compositing | RHI backend with parity tests, masks (Bezier, feather, ops), track mattes, nested comp UI, layer cache | Real-time 1080p preview targets met on reference GPU |
| M4 Creative studio | Colour studio (wheels, curves, LUT via OCIO), typography, preset browser, transition studio, audio-reactive bindings | — |
| M5 Professional | Tracking, roto (optional AI), optical flow module, nodes, plugin API (sandboxed), collect project | — |
| M6 Polish | Profiling, accessibility, shortcut editor, full benchmark suite | — |

## 17. Testing strategy

- **Unit tests** (implemented, `tests/`): keyframe math, velocity integrals vs closed form, every edit op with undo/redo state equality, serialization round-trip, migration from older schema, atomic save and recovery under simulated crash (truncated autosave), beat detection on synthetic click tracks at known BPM, blend-mode math, effect determinism (two renders bit-identical).
- **Integration tests** (implemented): generate real media with FFmpeg, probe it, decode exact frames (frame counter verified by colour), render a project, export MP4, re-probe the export (duration, fps, streams).
- **Golden images** [planned M3]: CPU reference renders checked in. GPU must match within tolerance.
- **Fuzzing** [planned]: project JSON parser and migrations (libFuzzer).
- **Sanitizers**: the CI matrix builds with ASan/UBSan (`-DKINETIC_SANITIZE=ON`).
- **Benchmarks** [planned]: fixed projects; results tracked per commit to catch regressions.
