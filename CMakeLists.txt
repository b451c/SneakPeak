cmake_minimum_required(VERSION 3.15)

if(APPLE)
  project(reaper_editview VERSION 0.1.0 LANGUAGES CXX OBJCXX)
else()
  project(reaper_editview VERSION 0.1.0 LANGUAGES CXX)
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

add_library(reaper_editview SHARED
  src/main.cpp
  src/globals.cpp
  src/edit_view.cpp
  src/waveform_view.cpp
  src/toolbar.cpp
  src/theme.cpp
  src/audio_engine.cpp
  src/audio_ops.cpp
  src/gain_panel.cpp
  src/marker_manager.cpp
  src/levels_panel.cpp
  src/spectral_view.cpp
)

target_include_directories(reaper_editview PRIVATE
  ${REAPER_SDK_DIR}
  ${WDL_DIR}
  ${WDL_DIR}/swell
  src
)

target_compile_definitions(reaper_editview PRIVATE
  $<$<NOT:$<PLATFORM_ID:Windows>>:SWELL_PROVIDED_BY_APP>
  $<$<CONFIG:Debug>:EDITVIEW_DEBUG>
  $<$<PLATFORM_ID:Windows>:NOMINMAX>
  WDL_NO_DEFINE_MINMAX
)

if(MSVC)
  target_compile_options(reaper_editview PRIVATE /W3)
else()
  target_compile_options(reaper_editview PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
    -Wshadow
    -Wconversion -Wno-sign-conversion
  )
endif()

if(APPLE)
  set_target_properties(reaper_editview PROPERTIES
    SUFFIX ".dylib"
    PREFIX ""
    OUTPUT_NAME "reaper_editview"
  )
  target_link_libraries(reaper_editview PRIVATE
    "-framework AppKit"
    "-framework CoreFoundation"
  )
  target_sources(reaper_editview PRIVATE
    ${WDL_DIR}/swell/swell-modstub.mm
  )
elseif(WIN32)
  set_target_properties(reaper_editview PROPERTIES
    SUFFIX ".dll"
    PREFIX ""
    OUTPUT_NAME "reaper_editview"
  )
  target_compile_definitions(reaper_editview PRIVATE
    WIN32_LEAN_AND_MEAN
  )
elseif(UNIX)
  set_target_properties(reaper_editview PROPERTIES
    SUFFIX ".so"
    PREFIX ""
    OUTPUT_NAME "reaper_editview"
  )
  target_sources(reaper_editview PRIVATE
    src/swell_modstub.cpp
  )
  target_link_libraries(reaper_editview PRIVATE
    dl
    pthread
  )
endif()
