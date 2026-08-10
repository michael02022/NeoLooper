# NeoLooper

A sample-looping audio editor inspired by Edison (Image-Line), built as a
native **CLAP** FX plugin with a **Dear ImGui** UI (X11/OpenGL on Linux,
Win32/WGL on Windows) — no JUCE. Everything lives in RAM; edits are
non-destructive until you hit Apply.

![screenshot](docs/screenshot.png)

## Features

- **Transport**: Play/Stop (space bar), Pause, Record (appends to the end of
  the loaded audio, adapting channels/sample rate). With a selection, Play
  starts and stops at its bounds; with a marked loop and no selection, it
  plays from the true start of the sample and the loop still governs where
  playback stops/wraps.
- **Load/Export**: WAV / AIFF / FLAC / OGG / MP3 (import only, no loop/root-
  note metadata) via drag & drop or the file browser. Export writes a WAV
  with an embedded `smpl` chunk (loop points, root note, finetune) at
  16-bit, 24-bit, or 32-bit float, auto-scaling the whole file (never
  clipping) if the chosen depth can't represent the peak.
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
`libfftw3-dev` and `libsamplerate0-dev`). CLAP, Dear ImGui, Loris and
Signalsmith Stretch are vendored under `third_party/`; Rubber Band links
against the system library via pkg-config.

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

## Build (Windows)

Dependencies: CMake ≥ 3.16, MSVC (Visual Studio 2022 Build Tools, "Desktop
development with C++" workload), and [vcpkg](https://github.com/microsoft/vcpkg)
for `libsndfile` and `rubberband` (pulls in `fftw3`/`libsamplerate`). CLAP,
Dear ImGui (including the Win32 backend, `third_party/imgui/backends/imgui_impl_win32.*`),
Loris/Signalsmith Stretch and dr_mp3 (MP3 import, see Features above) are
vendored the same way as on Linux.

```powershell
git clone https://github.com/microsoft/vcpkg C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg.exe install "libsndfile[core,external-libs]" rubberband --triplet x64-windows
```

`libsndfile`'s default features include an MPEG (MP3) codec via `mp3lame`/
`mpg123` -- explicitly excluded above (`[core,external-libs]` instead of the
default `[core,external-libs,mpeg]`) because NeoLooper never uses it (MP3
import goes through the vendored dr_mp3 instead) and because a DAW that
already bundles its own, differently-built `libmp3lame.dll`/`mpg123.dll`
(REAPER does, under its own `Plugins\` folder) can otherwise end up
resolving *our* `sndfile.dll`'s implicit import of those names to *its*
copy instead, which surfaces as a spurious "ordinal not found" error the
first time any audio file is opened. See `src/win_delayload.cpp` for the
underlying issue this also guards against more generally.

Then, from a **Developer Command Prompt/PowerShell for VS 2022** (needed for
`cl.exe`):

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build
```

`NeoLooper.clap` (a PE DLL despite the extension) and its vcpkg-provided
runtime DLLs (`sndfile.dll` deployed a second time as `neolooper_sndfile.dll`,
`rubberband-*.dll`, `FLAC.dll`, `ogg.dll`, `vorbis*.dll`, `opus.dll`,
`samplerate.dll`, `sleef*.dll`) land side by side in `build\`; copy the
whole set to wherever your host looks for CLAP plugins (e.g.
`%COMMONPROGRAMFILES%\CLAP` for all users, or
`%LOCALAPPDATA%\Programs\Common\CLAP` per-user), since those DLLs aren't
statically linked in. The plain `sndfile.dll` copy is a build byproduct and
isn't actually used at runtime (see below) -- harmless to leave out.

`sndfile.dll`/`rubberband-3.dll` are `/DELAYLOAD`-ed rather than linked
normally, and `src/win_delayload.cpp` resolves them (and `sndfile.dll`'s own
further dependencies) by an absolute path next to `NeoLooper.clap` itself
at first use: a host process has no reason to add a plugin's own directory
to its DLL search path, so a plain dynamic link would leave those DLLs
unfindable the moment the plugin is loaded from anywhere other than a
same-folder test harness. `sndfile.dll` is additionally deployed under the
plugin-private name `neolooper_sndfile.dll` (redirected to by that same
hook) since "sndfile.dll" is a common enough name that another already-
loaded component in the host process could collide with it.

### Tests

```powershell
build\core_test.exe %TEMP%
build\mini_host.exe build\NeoLooper.clap
build\mini_host.exe build\NeoLooper.clap gui
```

Also verified end-to-end inside REAPER on Windows: plugin scan/insert,
opening/closing the editor, importing WAV/MP3, and recording/loop
processing from the DAW.

## Project layout

```text
src/
  audio_data.hpp        planar float32 PCM in RAM + 16/32-bit semantics + peak pyramid
  document.{hpp,cpp}    editing, undo/redo, clipboard, envelopes, markers
  engine.{hpp,cpp}      audio thread: pitched playback (sinc16), recording, scope
  shared.hpp            lock-free GUI -> audio thread parameter snapshots
  resampler.{hpp,cpp}   offline high-quality sinc resampler
  sinc_interp.hpp       16-tap sinc interpolator for RT playback
  wav_io.{hpp,cpp}      libsndfile (WAV/AIFF/FLAC/OGG) + dr_mp3 (MP3) loading, WAV writer with smpl chunk
  plugin.cpp            CLAP glue (entry, ports, state, gui)
  gui/                  platform window (X11/GLX or Win32/WGL), toolbar/piano/scope, waveform view, file dialog
  loopmods/             one file per loop-processing module
  procmods/             one file per non-loop processor (Filter, Pitch, Stretch, ...)
```

## Status

Tested on Linux (X11) and, as of this writing, verified end-to-end on
Windows (Win32/WGL) inside REAPER: scan/insert, opening and closing the
plugin editor, importing audio (including MP3), and recording/loop
processing from the DAW. macOS is unported. The platform-specific surface
is small: `gui/gui_window_x11.cpp` / `gui/gui_window_win32.cpp` (X11/GLX/
XDND vs. Win32/WGL/`WM_DROPFILES`, sharing the public API in
`gui/gui_window.hpp`), `src/win_delayload.cpp` (Windows-only, resolves the
delay-loaded sndfile/rubberband DLLs by absolute path -- see Build
(Windows) above), the CLAP window API selection in `plugin.cpp`
(`CLAP_WINDOW_API_X11` vs. `_WIN32`), and a couple of one-line differences
in `gui/file_dialog.cpp` (home directory, case-insensitive sort). Porting
to macOS would mean adding a `gui_window_cocoa.mm` alongside the X11/Win32
pair.

## Credits

See [special_thanks.txt](special_thanks.txt) for the third-party algorithms
and libraries this project builds on.
