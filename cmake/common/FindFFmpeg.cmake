# FindFFmpeg.cmake
#
# Minimal component-based finder for the FFmpeg libraries shipped in the
# obs-deps prebuilt package. It searches CMAKE_PREFIX_PATH (which the OBS build
# presets point at the downloaded deps), so no system FFmpeg install is needed.
#
# Usage:
#   find_package(FFmpeg REQUIRED COMPONENTS avcodec avutil swscale)
#   target_link_libraries(mytarget PRIVATE FFmpeg::avcodec FFmpeg::avutil ...)
#
# Each requested component <c> yields an imported target FFmpeg::<c>.

include(FindPackageHandleStandardArgs)

# pkg-config gives us the right include/link flags on Linux/macOS when present.
find_package(PkgConfig QUIET)

set(_ffmpeg_required_vars)

foreach(_comp IN LISTS FFmpeg_FIND_COMPONENTS)
  set(_lib "lib${_comp}")

  if(PkgConfig_FOUND)
    pkg_check_modules(PC_${_comp} QUIET ${_lib})
  endif()

  find_path(
    FFmpeg_${_comp}_INCLUDE_DIR
    NAMES "${_lib}/version.h" "${_lib}/${_comp}.h"
    HINTS ${PC_${_comp}_INCLUDEDIR} ${PC_${_comp}_INCLUDE_DIRS}
    PATH_SUFFIXES include
  )

  find_library(
    FFmpeg_${_comp}_LIBRARY
    NAMES ${_comp} ${_lib}
    HINTS ${PC_${_comp}_LIBDIR} ${PC_${_comp}_LIBRARY_DIRS}
    PATH_SUFFIXES lib
  )

  list(APPEND _ffmpeg_required_vars
       FFmpeg_${_comp}_INCLUDE_DIR FFmpeg_${_comp}_LIBRARY)

  if(FFmpeg_${_comp}_INCLUDE_DIR AND FFmpeg_${_comp}_LIBRARY)
    # HANDLE_COMPONENTS keys off <pkg>_<comp>_FOUND - set it explicitly.
    set(FFmpeg_${_comp}_FOUND TRUE)
    if(NOT TARGET FFmpeg::${_comp})
      add_library(FFmpeg::${_comp} UNKNOWN IMPORTED)
      set_target_properties(
        FFmpeg::${_comp}
        PROPERTIES
          IMPORTED_LOCATION "${FFmpeg_${_comp}_LIBRARY}"
          INTERFACE_INCLUDE_DIRECTORIES "${FFmpeg_${_comp}_INCLUDE_DIR}"
      )
    endif()
  else()
    set(FFmpeg_${_comp}_FOUND FALSE)
  endif()

  mark_as_advanced(FFmpeg_${_comp}_INCLUDE_DIR FFmpeg_${_comp}_LIBRARY)
endforeach()

find_package_handle_standard_args(
  FFmpeg
  REQUIRED_VARS ${_ffmpeg_required_vars}
  HANDLE_COMPONENTS
)
