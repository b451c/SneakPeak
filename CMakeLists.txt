cmake_minimum_required(VERSION 3.24)  # Blend2D + AsmJit require >= 3.24

if(APPLE)
  project(reaper_sneakpeak VERSION 2.5.0 LANGUAGES CXX OBJCXX)
else()
  project(reaper_sneakpeak VERSION 2.5.0 LANGUAGES CXX)
endif()

# The release tag this build ships as ("v2.5.0-rc1"; CI passes the git ref
# name). Local builds are "dev" and the update check never offers them
# anything (audit A5.4).
set(SNEAKPEAK_BUILD_TAG "dev" CACHE STRING "release tag of this build (CI: the git ref name), dev for local builds")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# SDK path: supports both direct and cloned layouts
if(EXISTS "${CMAKE_SOURCE_DIR}/sdk/sdk/reaper_plugin.h")
  set(REAPER_SDK_DIR "${CMAKE_SOURCE_DIR}/sdk/sdk")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/sdk/reaper_plugin.h")
  set(REAPER_SDK_DIR "${CMAKE_SOURCE_DIR}/sdk")
else()
  message(FATAL_ERROR "REAPER SDK not found. Clone into cpp/sdk/: git clone https://github.com/justinfrankel/reaper-sdk.git cpp/sdk")
endif()
set(WDL_DIR "${CMAKE_SOURCE_DIR}/WDL/WDL")

# --- Blend2D (vector renderer for the premium UI redesign, branch feat/premium-ui) ---
# CPU rasteriser: no GPU/GL context, drops into our existing "fill 32-bit buffer
# -> BitBlt" model (see src/spectral_view.cpp). Built from source (plain C++, no
# extra toolchain). AsmJit (the JIT backend) is fetched alongside; Blend2D builds
# it itself via ASMJIT_DIR.
include(FetchContent)
FetchContent_Declare(asmjit
  GIT_REPOSITORY https://github.com/asmjit/asmjit.git
  GIT_TAG        0bd5787b54b575ed94bf32ac452153b34385c514  # pinned for reproducible CI
)
FetchContent_Declare(blend2d
  GIT_REPOSITORY https://github.com/blend2d/blend2d.git
  GIT_TAG        6dbc2cefbc996379e07104e34519a440b49b15d7  # pinned for reproducible CI
)
# Download AsmJit sources only; Blend2D add_subdirectory's them via ASMJIT_DIR.
FetchContent_GetProperties(asmjit)
if(NOT asmjit_POPULATED)
  FetchContent_Populate(asmjit)
endif()
set(ASMJIT_DIR "${asmjit_SOURCE_DIR}" CACHE PATH "AsmJit source dir" FORCE)
set(BLEND2D_STATIC TRUE CACHE BOOL "Build Blend2D as a static library" FORCE)
# Propagate the macOS deployment target into the sub-projects (the x86_64 CI job
# builds against 10.15; Blend2D/AsmJit JIT/SIMD code must target the same floor).
if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}" CACHE STRING "" FORCE)
endif()
FetchContent_MakeAvailable(blend2d)

# --- Embed UI fonts (Inter Medium/Regular, OFL-1.1) as C arrays at configure ---
# Keeps git clean (only the small TTFs are tracked) and ships a single self-
# contained dylib with no external font files to deploy.
set(SNEAKPEAK_FONT_GEN "${CMAKE_BINARY_DIR}/generated/embedded_fonts.cpp")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
file(WRITE "${SNEAKPEAK_FONT_GEN}" "// generated at configure time - do not edit\n")
function(_sp_embed_font varname path)
  file(READ "${path}" _hex HEX)
  string(REGEX REPLACE "(..)" "0x\\1," _bytes "${_hex}")
  file(APPEND "${SNEAKPEAK_FONT_GEN}"
       "extern const unsigned char ${varname}[] = {${_bytes}};\n"
       "extern const unsigned int ${varname}_size = sizeof(${varname});\n")
endfunction()
_sp_embed_font(g_fontInterMedium  "${CMAKE_SOURCE_DIR}/src/assets/fonts/Inter-Medium.ttf")
_sp_embed_font(g_fontInterRegular "${CMAKE_SOURCE_DIR}/src/assets/fonts/Inter-Regular.ttf")

add_library(reaper_sneakpeak SHARED
  src/main.cpp
  src/globals.cpp
  src/edit_view.cpp
  src/standalone_file.cpp
  src/item_audio_load.cpp
  src/dynamics_pipeline.cpp
  src/audio_commands.cpp
  src/context_menu.cpp
  src/drag_export.cpp
  src/rendering.cpp
  src/input_handling.cpp
  src/waveform_view.cpp
  src/waveform_rendering.cpp
  src/toolbar.cpp
  src/theme.cpp
  src/audio_engine.cpp
  src/audio_ops.cpp
  src/item_split_ops.cpp
  src/gain_panel.cpp
  src/marker_manager.cpp
  src/levels_panel.cpp
  src/spectral_view.cpp
  src/minimap_view.cpp
  src/multi_item_view.cpp
  src/dynamics_engine.cpp
  src/deess_engine.cpp
  src/dyn_trace.cpp
  src/dynamics_panel.cpp
  src/spectral_repair.cpp
  src/settings_panel.cpp
  src/limiter_engine.cpp
  src/limiter_panel.cpp
  src/oneshot_panel.cpp
  src/looplab_panel.cpp
  src/loop_finder.cpp
  src/wav_smpl.cpp
  src/wav_inplace.cpp
  src/wav_writer.cpp
  src/audio_stream.cpp
  src/view_stream.cpp
  src/export_stream.cpp
  src/ui_render.cpp
  ${SNEAKPEAK_FONT_GEN}
)

target_include_directories(reaper_sneakpeak PRIVATE
  ${REAPER_SDK_DIR}
  ${WDL_DIR}
  ${WDL_DIR}/swell
  src
)

target_compile_definitions(reaper_sneakpeak PRIVATE
  $<$<NOT:$<PLATFORM_ID:Windows>>:SWELL_PROVIDED_BY_APP>
  $<$<CONFIG:Debug>:SNEAKPEAK_DEBUG>
  $<$<PLATFORM_ID:Windows>:NOMINMAX>
  $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
  WDL_NO_DEFINE_MINMAX
  SNEAKPEAK_VERSION="${PROJECT_VERSION}"
  SNEAKPEAK_BUILD_TAG="${SNEAKPEAK_BUILD_TAG}"
)

if(MSVC)
  target_compile_options(reaper_sneakpeak PRIVATE /W3)
else()
  target_compile_options(reaper_sneakpeak PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
    -Wshadow
    -Wconversion -Wno-sign-conversion
    -fsigned-char
  )
endif()

# Blend2D: link the static lib (its interface adds -I to the headers root, so
# ui_render.cpp includes <blend2d/blend2d.h>). Header warnings are suppressed at
# the include site in ui_render.cpp.
target_link_libraries(reaper_sneakpeak PRIVATE blend2d::blend2d)

# PHASE 2: the premium Blend2D UI (Dynamics/Settings/gain/meters/toast). When ON,
# the GDI panel paths are replaced by the Blend2D renderers (OnPaintOverlay on the
# real window DC). Default ON since v2.2.0 (parity + global UI scale reached); the
# GDI fallback stays until Phase 3 deletes the #else paths + this option. Build the
# GDI fallback locally with: cmake -B build-off -DSNEAKPEAK_BLEND2D_PANEL=OFF
# ThreadSanitizer evidence build (audit A8.3, macOS only, never CI):
#   cmake -S . -B build-tsan -DSNEAKPEAK_TSAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
# REAPER must be launched with DYLD_INSERT_LIBRARIES=<libclang_rt.tsan_osx_dynamic.dylib>
# and TSAN_OPTIONS=suppressions=tests/tsan.supp:log_path=<file> (the runtime has to be
# in the process before the plugin loads). See .harness/plan_v250_audit_fixes.md A8.3.
option(SNEAKPEAK_TSAN "Build the plugin with -fsanitize=thread (evidence runs only)" OFF)
if(SNEAKPEAK_TSAN AND NOT MSVC)
  target_compile_options(reaper_sneakpeak PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(reaper_sneakpeak PRIVATE -fsanitize=thread)
endif()

option(SNEAKPEAK_BLEND2D_PANEL "Premium Blend2D UI (Dynamics/Settings/gain/meters/toast)" ON)
if(SNEAKPEAK_BLEND2D_PANEL)
  target_compile_definitions(reaper_sneakpeak PRIVATE SNEAKPEAK_BLEND2D_PANEL)
endif()

# --- v2.3.0 INC-0: offline envelope-diff regression harness ---
# Pure-DSP test binary (no REAPER/SWELL/Blend2D deps). Deterministic signals
# through DynamicsEngine; canonical dump byte-diffed against tests/baselines/
# by tests/run_dyn_regression.sh (or "record" to re-baseline intentionally).
add_executable(dyn_regression
  tests/dyn_regression.cpp
  src/dynamics_engine.cpp
  src/deess_engine.cpp
  src/dyn_trace.cpp
)
target_include_directories(dyn_regression PRIVATE src)
target_compile_definitions(dyn_regression PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)
if(MSVC)
  target_compile_options(dyn_regression PRIVATE /W3)
else()
  target_compile_options(dyn_regression PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
    -Wshadow
    -Wconversion -Wno-sign-conversion
    -fsigned-char
  )
endif()

# --- v2.5.0 A7.5: DynamicsParams string round trip + range clamp ---
# Pure test binary (no REAPER/SWELL/Blend2D deps), machine-independent (exit
# code). Run via tests/run_dyn_params_test.sh.
add_executable(dyn_params_test
  tests/dyn_params_test.cpp
  src/dynamics_engine.cpp
  src/dyn_trace.cpp
  src/deess_engine.cpp
)
target_include_directories(dyn_params_test PRIVATE src)
target_compile_definitions(dyn_params_test PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)
if(MSVC)
  target_compile_options(dyn_params_test PRIVATE /W3)
else()
  target_compile_options(dyn_params_test PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
    -Wshadow
    -Wconversion -Wno-sign-conversion
    -fsigned-char
  )
endif()

# --- v2.3.0 INC-5: offline spectral-repair correctness harness ---
# Pure-DSP test binary (no REAPER/SWELL/Blend2D deps). Synthetic click/beep
# scenarios through spectral_repair with SNR-improvement assertions (exit
# code), NOT bit-baselines - unlike dyn_regression it is machine-independent.
# Run via tests/run_spectral_repair_test.sh.
add_executable(spectral_repair_test
  tests/spectral_repair_test.cpp
  src/spectral_repair.cpp
)
target_include_directories(spectral_repair_test PRIVATE src)
target_compile_definitions(spectral_repair_test PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)
if(MSVC)
  target_compile_options(spectral_repair_test PRIVATE /W3)
else()
  target_compile_options(spectral_repair_test PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
    -Wshadow
    -Wconversion -Wno-sign-conversion
    -fsigned-char
  )
endif()

# --- v2.4.0 INC-L0: offline true-peak limiter harness ---
# Pure-DSP test binary (no REAPER/SWELL/Blend2D deps). Ceiling/passthrough/
# invariant assertions are machine-independent (exit code); the %.17g dump
# baseline is machine-local like dyn_regression. Run via
# tests/run_limiter_test.sh (or "record" to re-baseline intentionally).
add_executable(limiter_test
  tests/limiter_test.cpp
  src/limiter_engine.cpp
)
target_include_directories(limiter_test PRIVATE src)
target_compile_definitions(limiter_test PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)
if(MSVC)
  target_compile_options(limiter_test PRIVATE /W3)
else()
  target_compile_options(limiter_test PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
    -Wshadow
    -Wconversion -Wno-sign-conversion
    -fsigned-char
  )
endif()

# --- v2.5.0 audit A5.4: update-check version ordering (header-only, offline) ---
add_executable(version_compare_test tests/version_compare_test.cpp)
target_include_directories(version_compare_test PRIVATE src)
if(MSVC)
  target_compile_options(version_compare_test PRIVATE /W3)
else()
  target_compile_options(version_compare_test PRIVATE -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion)
endif()

# --- v2.5.0 audit A10: WAV format robustness (offline, hand-built files) ---
# Links the three WAV code paths SneakPeak owns (ReadWavHeader / WavInplace /
# WavWriter); audio_engine.cpp brings the REAPER function-pointer globals,
# all null here (the file paths under test never touch them).
add_executable(wav_format_test
  tests/wav_format_test.cpp
  src/audio_engine.cpp
  src/globals.cpp
  src/wav_writer.cpp
  src/wav_inplace.cpp
  src/wav_smpl.cpp
)
target_include_directories(wav_format_test PRIVATE ${REAPER_SDK_DIR} ${WDL_DIR} ${WDL_DIR}/swell src)
target_compile_definitions(wav_format_test PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)
if(WIN32)
  target_sources(wav_format_test PRIVATE src/win32_utf8_unit.c)
endif()
if(MSVC)
  target_compile_options(wav_format_test PRIVATE /W3)
else()
  # -Wno-unused-parameter: reaper_plugin.h (third-party, via audio_engine.h) has unused inline parameters
  target_compile_options(wav_format_test PRIVATE -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -Wno-unused-parameter)
endif()

# --- v2.4.0 INC-A2: offline loop-finder correctness check ---
# Pure-DSP test binary (no REAPER/SWELL/Blend2D deps), machine-independent
# assertions like spectral_repair_test. Run via tests/run_loop_finder_test.sh.
add_executable(loop_finder_test
  tests/loop_finder_test.cpp
  src/loop_finder.cpp
  src/wav_smpl.cpp
)
target_include_directories(loop_finder_test PRIVATE src)
target_compile_definitions(loop_finder_test PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)
if(MSVC)
  target_compile_options(loop_finder_test PRIVATE /W3)
else()
  target_compile_options(loop_finder_test PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
    -Wshadow
    -Wconversion -Wno-sign-conversion
    -fsigned-char
  )
endif()

if(APPLE)
  set_target_properties(reaper_sneakpeak PROPERTIES
    SUFFIX ".dylib"
    PREFIX ""
    OUTPUT_NAME "reaper_sneakpeak"
  )
  target_link_libraries(reaper_sneakpeak PRIVATE
    "-framework AppKit"
    "-framework CoreFoundation"
  )
  target_sources(reaper_sneakpeak PRIVATE
    ${WDL_DIR}/swell/swell-modstub.mm
  )
elseif(WIN32)
  set_target_properties(reaper_sneakpeak PROPERTIES
    SUFFIX ".dll"
    PREFIX ""
    OUTPUT_NAME "reaper_sneakpeak"
  )
  target_compile_definitions(reaper_sneakpeak PRIVATE
    WIN32_LEAN_AND_MEAN
  )
  # UTF-8 text/file/menu API wrappers (forum #63) - Windows only; SWELL is UTF-8
  # native. Wrapped in our own TU: WIN32_LEAN_AND_MEAN needs shellapi/commdlg
  # pulled in first, and the WDL checkout is upstream (no local patches).
  target_sources(reaper_sneakpeak PRIVATE
    src/win32_utf8_unit.c
  )
elseif(UNIX)
  set_target_properties(reaper_sneakpeak PROPERTIES
    SUFFIX ".so"
    PREFIX ""
    OUTPUT_NAME "reaper_sneakpeak"
  )
  target_sources(reaper_sneakpeak PRIVATE
    ${WDL_DIR}/swell/swell-modstub-generic.cpp
  )
  target_link_libraries(reaper_sneakpeak PRIVATE
    dl
    pthread
  )
endif()

# --- v2.5 8f: chunked-detector equivalence check ---
# Pure-DSP test binary (no REAPER/SWELL/Blend2D deps): DynTraceBuilder fed in
# chunks of every awkward size must equal one whole-buffer pass AND the legacy
# CollectPeaks / DeEssBandTrace loops bit for bit. Machine-independent (exact
# compares, exit code). Run via tests/run_dyn_chunk_test.sh.
add_executable(dyn_chunk_test
  tests/dyn_chunk_test.cpp
  src/dyn_trace.cpp
  src/deess_engine.cpp
)
target_include_directories(dyn_chunk_test PRIVATE src)
target_compile_definitions(dyn_chunk_test PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)
if(MSVC)
  target_compile_options(dyn_chunk_test PRIVATE /W3)
else()
  target_compile_options(dyn_chunk_test PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
    -Wshadow
    -Wconversion -Wno-sign-conversion
    -fsigned-char
  )
endif()
