cmake_minimum_required(VERSION 3.16)
project(NeoLooper VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
# Rubber Band Library (breakfastquay.com/rubberband, GPLv2+ -- same
# licensing situation this project already has with the vendored Loris,
# see third_party/loris-2.0/COPYING): linked as a system library, same
# criterion as sndfile/X11/OpenGL below, rather than vendored, since it's
# packaged by every major distro (Debian/Ubuntu: librubberband-dev) and
# isn't the kind of niche/hard-to-package dependency Loris/ImGui/CLAP are.
pkg_check_modules(RUBBERBAND REQUIRED IMPORTED_TARGET rubberband)
find_package(X11 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# Signalsmith Stretch (signalsmith-audio.co.uk/code/stretch), header-only,
# MIT: vendored under third_party/signalsmith-stretch (see its
# VENDORED_FROM.txt) since it's not commonly packaged by distros, same
# criterion as ImGui/CLAP.
set(SIGNALSMITH_STRETCH_DIR ${CMAKE_SOURCE_DIR}/third_party/signalsmith-stretch)

# Icon font (Font Awesome 4, ver third_party/fontawesome/VENDORED_FROM.txt)
# embebido en el binario via xxd -i: el .clap sigue siendo un unico archivo
# autocontenido, sin depender de una fuente instalada en el sistema del
# usuario ni de distribuir un .ttf suelto junto al plugin.
find_program(XXD_EXECUTABLE xxd REQUIRED)
set(FONTAWESOME_DIR ${CMAKE_SOURCE_DIR}/third_party/fontawesome)
set(FONTAWESOME_DATA_CPP ${CMAKE_BINARY_DIR}/generated/fontawesome_data.cpp)
add_custom_command(
  OUTPUT ${FONTAWESOME_DATA_CPP}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
  COMMAND ${XXD_EXECUTABLE} -i fontawesome-webfont.ttf ${FONTAWESOME_DATA_CPP}
  WORKING_DIRECTORY ${FONTAWESOME_DIR}
  DEPENDS ${FONTAWESOME_DIR}/fontawesome-webfont.ttf
  COMMENT "Embebiendo fontawesome-webfont.ttf (xxd -i)"
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
# own CMakeLists.txt uses for its `loris` target.
set_source_files_properties(${LORIS_SRC} PROPERTIES COMPILE_OPTIONS -Wno-comment)

add_library(neolooper MODULE
  src/plugin.cpp
  src/document.cpp
  src/engine.cpp
  src/resampler.cpp
  src/wav_io.cpp
  src/foreign_metadata.cpp
  src/gui/gui_window.cpp
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
  PkgConfig::SNDFILE
  PkgConfig::RUBBERBAND
  X11::X11
  OpenGL::GL
  Threads::Threads
)

set_target_properties(neolooper PROPERTIES
  PREFIX ""
  SUFFIX ".clap"
  OUTPUT_NAME "NeoLooper"
)

# Host de prueba minimo: carga el .clap, lo instancia y procesa audio sin GUI.
add_executable(mini_host test/mini_host.c)
target_include_directories(mini_host PRIVATE third_party/clap/include)
target_link_libraries(mini_host PRIVATE ${CMAKE_DL_LIBS} m)

# Test del nucleo offline (Document, Engine RT, resampler, WAV+smpl).
add_executable(core_test
  test/core_test.cpp
  src/document.cpp
  src/engine.cpp
  src/resampler.cpp
  src/wav_io.cpp
  src/foreign_metadata.cpp
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
target_include_directories(core_test PRIVATE src ${LORIS_DIR} ${SIGNALSMITH_STRETCH_DIR})
target_link_libraries(core_test PRIVATE PkgConfig::SNDFILE PkgConfig::RUBBERBAND Threads::Threads m)
