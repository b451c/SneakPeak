cmake_minimum_required(VERSION 3.24)  # Blend2D + AsmJit require >= 3.24

if(APPLE)
  project(reaper_sneakpeak VERSION 2.1.1 LANGUAGES CXX OBJCXX)
else()
  project(reaper_sneakpeak VERSION 2.1.1 LANGUAGES CXX)
endif()

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
  src/dynamics_panel.cpp
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

# PHASE 0 ONLY: the SNEAKPEAK_UI_SPIKE demo tile that de-risks the Blend2D
# renderer. REMOVE this option + the SNEAKPEAK_UI_SPIKE code (ui_render spike,
# DrawUiSpike, the OnPaint call) once Phase 0 is complete.
option(SNEAKPEAK_UI_SPIKE_PHASE0 "Phase 0 Blend2D de-risk demo tile (remove after Phase 0)" ON)
if(SNEAKPEAK_UI_SPIKE_PHASE0)
  target_compile_definitions(reaper_sneakpeak PRIVATE SNEAKPEAK_UI_SPIKE)
endif()

# PHASE 2: the premium Blend2D Dynamics Panel. When ON, the GDI DynamicsPanel::Draw
# is replaced by DrawPremium (rendered in OnPaintOverlay on the real window DC).
# Default OFF until the premium panel reaches feature parity with the GDI panel
# (then flip ON, and in Phase 3 delete the GDI #else path + this option). Build the
# premium panel locally with: cmake -B build -DSNEAKPEAK_BLEND2D_PANEL=ON
option(SNEAKPEAK_BLEND2D_PANEL "Phase 2 premium Blend2D Dynamics Panel (replaces GDI when parity reached)" OFF)
if(SNEAKPEAK_BLEND2D_PANEL)
  target_compile_definitions(reaper_sneakpeak PRIVATE SNEAKPEAK_BLEND2D_PANEL)
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
