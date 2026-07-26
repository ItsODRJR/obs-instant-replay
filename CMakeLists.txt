cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# This plugin needs the frontend API (scene switching, hotkeys, program output
# access), FFmpeg (decoding replay clips out of the encoded RAM ring buffer),
# and Qt (the operator dock: toggle button, status, scene selection).
option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

# Let FindFFmpeg.cmake (shipped alongside the other common modules) be located.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/common")

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# FFmpeg is provided by the obs-deps prebuilt package (same prefix as libobs).
find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil swscale)
target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale
)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/ring-buffer.cpp
    src/replay-output.cpp
    src/decoder.cpp
    src/replay-source.cpp
    src/replay-controller.cpp
)

# Qt operator dock (built only when Qt is enabled).
if(ENABLE_QT)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/replay-dock.cpp src/replay-dock.hpp)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAVE_QT_DOCK)
endif()

target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
