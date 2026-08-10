cmake_minimum_required(VERSION 3.16)
project(NeoLooper VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
  # M_PI et al. aren't defined by <cmath>/<math.h> under MSVC otherwise --
  # used throughout src/ and test/ (Linux's libc defines them unconditionally).
  add_compile_definitions(_USE_MATH_DEFINES)
endif()

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# Rubber Band Library (breakfastquay.com/rubberband, GPLv2+ -- same
# licensing situation this project already has with the vendored Loris, see
# third_party/loris-2.0/COPYING): linked as a system library rather than
# vendored, since it's packaged by every major distro (Debian/Ubuntu:
# librubberband-dev) and by vcpkg on Windows, and isn't the kind of niche/
# hard-to-package dependency Loris/ImGui/CLAP are.
if(WIN32)
  # vcpkg triplet: sndfile ships a CMake config (SndFile::sndfile). rubberband's
  # doesn't reliably across vcpkg versions, so fall back to a manual
  # find_library/find_path IMPORTED target when no config package is found.
  find_package(SndFile CONFIG REQUIRED)
  find_package(rubberband CONFIG QUIET)
  if(NOT TARGET RubberBand::rubberband)
    find_path(RUBBERBAND_INCLUDE_DIR NAMES rubberband/RubberBandStretcher.h)
    find_library(RUBBERBAND_LIBRARY NAMES rubberband)
    if(NOT RUBBERBAND_INCLUDE_DIR OR NOT RUBBERBAND_LIBRARY)
      message(FATAL_ERROR "Rubber Band not found -- vcpkg install rubberband")
    endif()
    add_library(RubberBand::rubberband UNKNOWN IMPORTED)
    set_target_properties(RubberBand::rubberband PROPERTIES
      IMPORTED_LOCATION "${RUBBERBAND_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${RUBBERBAND_INCLUDE_DIR}")
  endif()

  # FLAC (flac_io.cpp: libFLAC's metadata-chain API embeds loop/cue chunks
  # into the FLAC files libsndfile writes) + Vorbis/vorbisenc/Ogg (ogg_io.cpp:
  # hand-rolled encoder, since libsndfile can't write custom Vorbis comment
  # tags like LOOPSTART/LOOPLENGTH). Same defensive CONFIG-then-find_library
  # fallback as rubberband above -- vcpkg's exported target names for these
  # three have varied across versions.
  find_package(FLAC CONFIG QUIET)
  if(NOT TARGET FLAC::FLAC)
    find_path(FLAC_INCLUDE_DIR NAMES FLAC/metadata.h)
    find_library(FLAC_LIBRARY NAMES FLAC libFLAC)
    if(NOT FLAC_INCLUDE_DIR OR NOT FLAC_LIBRARY)
      message(FATAL_ERROR "FLAC not found -- vcpkg install libflac")
    endif()
    add_library(FLAC::FLAC UNKNOWN IMPORTED)
    set_target_properties(FLAC::FLAC PROPERTIES
      IMPORTED_LOCATION "${FLAC_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${FLAC_INCLUDE_DIR}")
  endif()

  find_package(Ogg CONFIG QUIET)
  if(NOT TARGET Ogg::ogg)
    find_path(OGG_INCLUDE_DIR NAMES ogg/ogg.h)
    find_library(OGG_LIBRARY NAMES ogg)
    if(NOT OGG_INCLUDE_DIR OR NOT OGG_LIBRARY)
      message(FATAL_ERROR "Ogg not found -- vcpkg install libogg")
    endif()
    add_library(Ogg::ogg UNKNOWN IMPORTED)
    set_target_properties(Ogg::ogg PROPERTIES
      IMPORTED_LOCATION "${OGG_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${OGG_INCLUDE_DIR}")
  endif()

  find_package(Vorbis CONFIG QUIET)
  if(NOT TARGET Vorbis::vorbis OR NOT TARGET Vorbis::vorbisenc OR NOT TARGET Vorbis::vorbisfile)
    find_path(VORBIS_INCLUDE_DIR NAMES vorbis/codec.h)
    find_library(VORBIS_LIBRARY NAMES vorbis)
    find_library(VORBISENC_LIBRARY NAMES vorbisenc)
    find_library(VORBISFILE_LIBRARY NAMES vorbisfile)
    if(NOT VORBIS_INCLUDE_DIR OR NOT VORBIS_LIBRARY OR NOT VORBISENC_LIBRARY OR NOT VORBISFILE_LIBRARY)
      message(FATAL_ERROR "Vorbis not found -- vcpkg install libvorbis")
    endif()
    add_library(Vorbis::vorbis UNKNOWN IMPORTED)
    set_target_properties(Vorbis::vorbis PROPERTIES
      IMPORTED_LOCATION "${VORBIS_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${VORBIS_INCLUDE_DIR}")
    add_library(Vorbis::vorbisenc UNKNOWN IMPORTED)
    set_target_properties(Vorbis::vorbisenc PROPERTIES
      IMPORTED_LOCATION "${VORBISENC_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${VORBIS_INCLUDE_DIR}")
    add_library(Vorbis::vorbisfile UNKNOWN IMPORTED)
    set_target_properties(Vorbis::vorbisfile PROPERTIES
      IMPORTED_LOCATION "${VORBISFILE_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${VORBIS_INCLUDE_DIR}")
  endif()
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
  pkg_check_modules(RUBBERBAND REQUIRED IMPORTED_TARGET rubberband)
  pkg_check_modules(FLAC REQUIRED IMPORTED_TARGET flac)
  pkg_check_modules(VORBIS REQUIRED IMPORTED_TARGET vorbis)
  pkg_check_modules(VORBISENC REQUIRED IMPORTED_TARGET vorbisenc)
  pkg_check_modules(VORBISFILE REQUIRED IMPORTED_TARGET vorbisfile)
  pkg_check_modules(OGG REQUIRED IMPORTED_TARGET ogg)
  find_package(X11 REQUIRED)
endif()

if(WIN32)
  set(SNDFILE_TARGET SndFile::sndfile)
  set(RUBBERBAND_TARGET RubberBand::rubberband)
  set(FLAC_TARGET FLAC::FLAC)
  set(VORBIS_TARGET Vorbis::vorbis)
  set(VORBISENC_TARGET Vorbis::vorbisenc)
  set(VORBISFILE_TARGET Vorbis::vorbisfile)
  set(OGG_TARGET Ogg::ogg)
  set(M_LIB "")
else()
  set(SNDFILE_TARGET PkgConfig::SNDFILE)
  set(RUBBERBAND_TARGET PkgConfig::RUBBERBAND)
  set(FLAC_TARGET PkgConfig::FLAC)
  set(VORBIS_TARGET PkgConfig::VORBIS)
  set(VORBISENC_TARGET PkgConfig::VORBISENC)
  set(VORBISFILE_TARGET PkgConfig::VORBISFILE)
  set(OGG_TARGET PkgConfig::OGG)
  set(M_LIB m)
endif()

# Signalsmith Stretch (signalsmith-audio.co.uk/code/stretch), header-only,
# MIT: vendored under third_party/signalsmith-stretch (see its
# VENDORED_FROM.txt) since it's not commonly packaged by distros, same
# criterion as ImGui/CLAP.
set(SIGNALSMITH_STRETCH_DIR ${CMAKE_SOURCE_DIR}/third_party/signalsmith-stretch)

# Icon font (Font Awesome 4, ver third_party/fontawesome/VENDORED_FROM.txt)
# embebido en el binario: el .clap sigue siendo un unico archivo
# autocontenido, sin depender de una fuente instalada en el sistema del
# usuario ni de distribuir un .ttf suelto junto al plugin. bin2c
# (tools/bin2c.cpp) reemplaza a `xxd -i` -- no esta disponible en Windows,
# y esto evita la dependencia de una herramienta externa tambien en Linux.
add_executable(bin2c tools/bin2c.cpp)

set(FONTAWESOME_DIR ${CMAKE_SOURCE_DIR}/third_party/fontawesome)
set(FONTAWESOME_DATA_CPP ${CMAKE_BINARY_DIR}/generated/fontawesome_data.cpp)
add_custom_command(
  OUTPUT ${FONTAWESOME_DATA_CPP}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
  COMMAND $<TARGET_FILE:bin2c> ${FONTAWESOME_DIR}/fontawesome-webfont.ttf
          ${FONTAWESOME_DATA_CPP} fontawesome_webfont_ttf
  DEPENDS bin2c ${FONTAWESOME_DIR}/fontawesome-webfont.ttf
  COMMENT "Embebiendo fontawesome-webfont.ttf (bin2c)"
  VERBATIM
)

# Loris (third_party/loris-2.0): additive sinusoidal-partial analysis/
# resynthesis, used by procmods/loris_dsp for the "Loris" transpose module.
# Only the C++ class library, not the SWIG/C procedural interface (loris.h,
# the lorisNonObj_pi.cpp-and-friends bridge) -- we call Analyzer/Channelizer/
# Distiller/PartialUtils/Synthesizer directly instead. Also skipped: file
# I/O the plugin doesn't need (Aiff/Sdif/Spc/ImportLemur -- NeoLooper has
# its own WAV I/O), Morpher/Harmonifier (cross-synthesis features out of
# scope), and fast-synth-src (a separate, unused synthesis path). Compiled
# straight into neolooper/core_test, same as third_party/imgui below,
# rather than via Loris's own CMakeLists.txt (which hardcodes a SHARED
# library target -- unsuitable for a self-contained .clap).
set(LORIS_DIR ${CMAKE_SOURCE_DIR}/third_party/loris-2.0/src)
set(LORIS_SRC
  ${LORIS_DIR}/Analyzer.cpp
  ${LORIS_DIR}/AssociateBandwidth.cpp
  ${LORIS_DIR}/Breakpoint.cpp
  ${LORIS_DIR}/BreakpointUtils.cpp
  ${LORIS_DIR}/Channelizer.cpp
  ${LORIS_DIR}/Collator.cpp
  ${LORIS_DIR}/Dilator.cpp
  ${LORIS_DIR}/Distiller.cpp
  ${LORIS_DIR}/Envelope.cpp
  ${LORIS_DIR}/F0Estimate.cpp
  ${LORIS_DIR}/LorisExceptions.cpp
  ${LORIS_DIR}/Filter.cpp
  ${LORIS_DIR}/FourierTransform.cpp
  ${LORIS_DIR}/FrequencyReference.cpp
  ${LORIS_DIR}/Fundamental.cpp
  ${LORIS_DIR}/KaiserWindow.cpp
  ${LORIS_DIR}/LinearEnvelope.cpp
  ${LORIS_DIR}/Marker.cpp
  ${LORIS_DIR}/NoiseGenerator.cpp
  ${LORIS_DIR}/Notifier.cpp
  ${LORIS_DIR}/Oscillator.cpp
  ${LORIS_DIR}/Partial.cpp
  ${LORIS_DIR}/PartialBuilder.cpp
  ${LORIS_DIR}/PartialList.cpp
  ${LORIS_DIR}/PartialUtils.cpp
  ${LORIS_DIR}/phasefix.cpp
  ${LORIS_DIR}/ReassignedSpectrum.cpp
  ${LORIS_DIR}/Resampler.cpp
  ${LORIS_DIR}/Sieve.cpp
  ${LORIS_DIR}/SpectralPeakSelector.cpp
  ${LORIS_DIR}/SpectralSurface.cpp
  ${LORIS_DIR}/Synthesizer.cpp
  ${LORIS_DIR}/fftsg.c
)
# Loris's own headers trip -Wcomment (doxygen "//!" blocks containing "/*"-
# looking text); silence it only for these vendored files, same flag Loris's
# own CMakeLists.txt uses for its `loris` target. MSVC doesn't have this
# warning (nor the -W flag syntax), so the option only applies on GCC/Clang.
set_source_files_properties(${LORIS_SRC} PROPERTIES COMPILE_OPTIONS
  "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-comment>")

# GUI windowing/event backend: X11+GLX on Linux, Win32+WGL on Windows -- the
# only platform-specific pair of implementations in the codebase, sharing a
# single public API (src/gui/gui_window.hpp). See src/gui/gui_window_x11.cpp
# and src/gui/gui_window_win32.cpp for the split rationale.
if(WIN32)
  set(GUI_WINDOW_SRC
    src/gui/gui_window_win32.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
  )
  # A loaded-as-a-plugin DLL can't influence the DLL search path the host
  # process uses for its own imports (that search is anchored to the HOST's
  # directory, not the plugin's) -- so vcpkg's sndfile.dll/rubberband-3.dll
  # sitting next to NeoLooper.clap wouldn't be found when e.g. REAPER loads
  # it, even though they resolve fine from a same-folder test harness.
  # Delay-loading them and resolving the path ourselves (src/win_delayload.cpp)
  # sidesteps host-controlled search order entirely.
  set(WIN_DELAYLOAD_SRC src/win_delayload.cpp)
else()
  set(GUI_WINDOW_SRC src/gui/gui_window_x11.cpp)
  set(WIN_DELAYLOAD_SRC)
endif()

add_library(neolooper MODULE
  src/plugin.cpp
  src/document.cpp
  src/engine.cpp
  src/resampler.cpp
  src/wav_io.cpp
  src/foreign_metadata.cpp
  src/riff_chunks.cpp
  src/flac_io.cpp
  src/ogg_io.cpp
  src/aiff_io.cpp
  ${GUI_WINDOW_SRC}
  ${WIN_DELAYLOAD_SRC}
  src/gui/editor_ui.cpp
  src/gui/waveform_view.cpp
  src/gui/file_dialog.cpp
  src/loopmods/loop_module.cpp
  src/loopmods/classic_crossfade.cpp
  src/loopmods/classic_crossfade_inside.cpp
  src/loopmods/crossfade_inside_core.cpp
  src/loopmods/pingpong_crossfade.cpp
  src/loopmods/pingpong_crossfade_inside.cpp
  src/loopmods/pingpong_classic.cpp
  src/loopmods/harmonic_loop.cpp
  src/loopmods/harmonic_loop_dsp.cpp
  src/loopmods/spectral_loop_core.cpp
  src/loopmods/freeze_loop.cpp
  src/loopmods/freeze_loop_dsp.cpp
  src/loopmods/freeze_stretch.cpp
  src/loopmods/freeze_stretch_dsp.cpp
  src/loopmods/rotation_loop.cpp
  src/loopmods/rotation_loop_dsp.cpp
  src/loopmods/granular_loop.cpp
  src/loopmods/granular_loop_dsp.cpp
  src/loopmods/pingpong_harmonic.cpp
  src/loopmods/pingpong_harmonic_dsp.cpp
  src/loopmods/auto_loop_finder.cpp
  src/loopmods/auto_loop_milselarch.cpp
  src/loopmods/loopauditioneer_finder.cpp
  src/loopmods/auto_loop_loopauditioneer.cpp
  src/procmods/filter.cpp
  src/procmods/filter_dsp.cpp
  src/procmods/amplitude.cpp
  src/procmods/amplitude_dsp.cpp
  src/procmods/pitch_dsp.cpp
  src/procmods/pitch_method.cpp
  src/procmods/pitch_builtin.cpp
  src/procmods/pitch_signalsmith_dsp.cpp
  src/procmods/pitch_signalsmith.cpp
  src/procmods/pitch_loris.cpp
  src/procmods/stretch_method.cpp
  src/procmods/stretch_dsp.cpp
  src/procmods/stretch_builtin.cpp
  src/procmods/rubberband_stretch_dsp.cpp
  src/procmods/stretch_rubberband.cpp
  src/procmods/signalsmith_stretch_dsp.cpp
  src/procmods/stretch_signalsmith.cpp
  src/procmods/time_stretch_core.cpp
  src/procmods/loris_dsp.cpp
  ${LORIS_SRC}
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_opengl2.cpp
  ${FONTAWESOME_DATA_CPP}
)

target_include_directories(neolooper PRIVATE
  src
  third_party
  third_party/clap/include
  third_party/imgui
  third_party/fontawesome
  ${LORIS_DIR}
  ${SIGNALSMITH_STRETCH_DIR}
)

target_link_libraries(neolooper PRIVATE
  ${SNDFILE_TARGET}
  ${RUBBERBAND_TARGET}
  ${FLAC_TARGET}
  ${VORBIS_TARGET}
  ${VORBISENC_TARGET}
  ${VORBISFILE_TARGET}
  ${OGG_TARGET}
  OpenGL::GL
  Threads::Threads
)
if(WIN32)
  # gdi32: WGL pixel format + context; shell32: WM_DROPFILES (DragAcceptFiles/
  # DragQueryFile); user32: window/message-loop APIs; delayimp: runtime
  # support for the /DELAYLOAD imports resolved in src/win_delayload.cpp.
  target_link_libraries(neolooper PRIVATE gdi32 shell32 user32 delayimp)
  # FLAC/ogg/vorbis/vorbisenc/vorbisfile are now DIRECT imports (flac_io.cpp/
  # ogg_io.cpp), not just sndfile.dll's transitive dependencies --
  # src/win_delayload.cpp already primes all of these as "sibling libs"
  # before sndfile.dll loads, but that priming only helps if they're ALSO
  # delay-loaded themselves; otherwise the OS resolves them at plugin-load
  # time via the host's own (wrong) search path, same problem
  # sndfile.dll/rubberband-3.dll had.
  target_link_options(neolooper PRIVATE
    /DELAYLOAD:sndfile.dll /DELAYLOAD:rubberband-3.dll
    /DELAYLOAD:FLAC.dll /DELAYLOAD:ogg.dll /DELAYLOAD:vorbis.dll
    /DELAYLOAD:vorbisenc.dll /DELAYLOAD:vorbisfile.dll)
  # "sndfile.dll" is a common name other audio software might also bundle;
  # ship our copy under a plugin-unique name so a host process that loads
  # many plugins together (a DAW) can never resolve someone else's
  # same-named DLL to ours or vice versa. src/win_delayload.cpp redirects
  # the linker's expected "sndfile.dll" name to this file at load time.
  add_custom_command(TARGET neolooper POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:SndFile::sndfile>"
            "$<TARGET_FILE_DIR:neolooper>/neolooper_sndfile.dll"
    COMMENT "Copiando sndfile.dll con nombre unico (neolooper_sndfile.dll)"
  )
else()
  target_link_libraries(neolooper PRIVATE X11::X11)
endif()

set_target_properties(neolooper PROPERTIES
  PREFIX ""
  SUFFIX ".clap"
  OUTPUT_NAME "NeoLooper"
)

# Host de prueba minimo: carga el .clap, lo instancia y procesa audio sin GUI.
add_executable(mini_host test/mini_host.c)
target_include_directories(mini_host PRIVATE third_party/clap/include)
if(NOT WIN32)
  target_link_libraries(mini_host PRIVATE ${CMAKE_DL_LIBS} m)
endif()

# Test del nucleo offline (Document, Engine RT, resampler, WAV+smpl).
add_executable(core_test
  test/core_test.cpp
  src/document.cpp
  src/engine.cpp
  src/resampler.cpp
  src/wav_io.cpp
  src/foreign_metadata.cpp
  src/riff_chunks.cpp
  src/flac_io.cpp
  src/ogg_io.cpp
  src/aiff_io.cpp
  src/loopmods/harmonic_loop_dsp.cpp
  src/loopmods/spectral_loop_core.cpp
  src/loopmods/freeze_loop_dsp.cpp
  src/loopmods/freeze_stretch_dsp.cpp
  src/loopmods/rotation_loop_dsp.cpp
  src/loopmods/granular_loop_dsp.cpp
  src/loopmods/crossfade_inside_core.cpp
  src/loopmods/pingpong_harmonic_dsp.cpp
  src/loopmods/auto_loop_finder.cpp
  src/loopmods/loopauditioneer_finder.cpp
  src/procmods/filter_dsp.cpp
  src/procmods/amplitude_dsp.cpp
  src/procmods/pitch_dsp.cpp
  src/procmods/pitch_signalsmith_dsp.cpp
  src/procmods/stretch_dsp.cpp
  src/procmods/rubberband_stretch_dsp.cpp
  src/procmods/signalsmith_stretch_dsp.cpp
  src/procmods/time_stretch_core.cpp
  src/procmods/loris_dsp.cpp
  ${LORIS_SRC}
)
target_include_directories(core_test PRIVATE src third_party ${LORIS_DIR} ${SIGNALSMITH_STRETCH_DIR})
target_link_libraries(core_test PRIVATE
  ${SNDFILE_TARGET} ${RUBBERBAND_TARGET} ${FLAC_TARGET} ${VORBIS_TARGET} ${VORBISENC_TARGET}
  ${VORBISFILE_TARGET} ${OGG_TARGET}
  Threads::Threads ${M_LIB})
