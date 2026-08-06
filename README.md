# NeoLooper

A sample-looping audio editor inspired by Edison (Image-Line), built as a
native **CLAP** FX plugin with a **Dear ImGui** UI (X11/OpenGL) — no JUCE.
Everything lives in RAM; edits are non-destructive until you hit Apply.

![screenshot](docs/screenshot.png)

## Features

- **Transport**: Play/Stop (space bar), Pause, Record (appends to the end of
  the loaded audio, adapting channels/sample rate). With a selection, Play
  starts and stops at its bounds; with a marked loop and no selection, it
  plays from the true start of the sample and the loop still governs where
  playback stops/wraps.
- **Load/Export**: WAV / AIFF / FLAC / OGG via drag & drop or the file
  browser. Export writes a WAV with an embedded `smpl` chunk (loop points,
  root note, finetune) at 16-bit, 24-bit, or 32-bit float, auto-scaling the
  whole file (never clipping) if the chosen depth can't represent the peak.
- **16/32-bit editing** (Edison-style): edits quantize to the 16-bit grid,
  auto-promoting to 32-bit if an edit would clip. Manual 32→16 toggle scales
  the whole sample down instead of clipping it.
- Sample rate change (simple or high-quality sinc resample), stereo→mono,
  normalize, cut/copy/paste/delete, trim tail / remove attack, undo/redo,
  and DAW project state save/restore.
- **Waveform view** with scroll-wheel zoom, sample-accurate selection, and
  draggable loop markers (Edison-style drag behavior).
- **Auto Loop**: a menu of pluggable algorithms that find a repeatable
  region and place the loop points automatically, searching the selection
  (or the whole sample if there's none):
  - [milselarch/autolooper](https://github.com/milselarch/autolooper)
  - [GrandOrgue/LoopAuditioneer](https://github.com/GrandOrgue/LoopAuditioneer) (Lars Palo)
- **Volume/Pan/Separation envelopes**, audible live before being printed to
  the sample.
- **128-key piano** (assign root note, audition relative pitch) with a
  ±99-cent finetune slider, plus a tuning oscilloscope.
- **High-quality real-time playback**: a 16-tap sinc (Kaiser window)
  interpolator, wrapped correctly across loop boundaries.

### Process menu

Each entry opens its own window with a live, non-destructive preview;
nothing is written to the document until you hit Apply.

- **Loop modules** — one file per loop type, each doing time-domain
  crossfade work to make a marked loop repeat seamlessly:
  Classic Crossfade (+ Inside variant), Ping Pong Crossfade (+ Inside),
  Ping Pong Classic (historically-accurate ping-pong construction),
  Harmonic, Ping Pong Harmonic, Freeze, FreezeStretch (ambiance texture via
  Signalsmith Stretch pushed past its clean-stretch range), Rotation,
  Granular.
- **Filter**: resonant high-pass + bell band over the real spectrum.
- **Amplitude**: plain gain in dB, with an optional hard clip.
- **Pitch**: built-in (resample or phase-vocoder),
  [Signalsmith](https://signalsmith-audio.co.uk/code/stretch/) (tonality
  limit + formant preservation), or Loris (additive sinusoidal-partial
  resynthesis — best timbre preservation for monophonic, quasi-harmonic
  material).
- **Stretch**: built-in phase vocoder,
  [Rubber Band](https://breakfastquay.com/rubberband/), or Signalsmith.

## Build (Linux)

Dependencies: CMake ≥ 3.16, a C++20 compiler, `libsndfile1-dev`,
`libx11-dev`, `libgl1-mesa-dev`, `librubberband-dev` (pulls in
`libfftw3-dev` and `libsamplerate0-dev`), and `xxd` (usually preinstalled;
otherwise `apt install xxd`). CLAP, Dear ImGui, Loris and Signalsmith
Stretch are vendored under `third_party/`; Rubber Band links against the
system library via pkg-config.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cp build/NeoLooper.clap ~/.clap/
```

### Tests

```sh
./build/core_test /tmp                        # offline core + RT engine checks
./build/mini_host build/NeoLooper.clap         # headless smoke test
./build/mini_host build/NeoLooper.clap gui     # + a real 4s GUI window
```

## Project layout

```text
src/
  audio_data.hpp        planar float32 PCM in RAM + 16/32-bit semantics + peak pyramid
  document.{hpp,cpp}    editing, undo/redo, clipboard, envelopes, markers
  engine.{hpp,cpp}      audio thread: pitched playback (sinc16), recording, scope
  shared.hpp            lock-free GUI -> audio thread parameter snapshots
  resampler.{hpp,cpp}   offline high-quality sinc resampler
  sinc_interp.hpp       16-tap sinc interpolator for RT playback
  wav_io.{hpp,cpp}      libsndfile loading + WAV writer with smpl chunk
  plugin.cpp            CLAP glue (entry, ports, state, gui)
  gui/                  X11/GLX window, toolbar/piano/scope, waveform view, file dialog
  loopmods/             one file per loop-processing module
  procmods/             one file per non-loop processor (Filter, Pitch, Stretch, ...)
```

## Status

Tested on Linux (X11). The only platform-specific code is `gui/gui_window.cpp`
(X11/GLX/XDND); porting to Windows/macOS would mean replacing that file.

## Credits

See [special_thanks.txt](special_thanks.txt) for the third-party algorithms
and libraries this project builds on.
