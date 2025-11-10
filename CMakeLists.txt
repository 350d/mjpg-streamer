cmake_minimum_required(VERSION 3.10)
set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/src/cmake ${CMAKE_MODULE_PATH})

# Ensure mjpg_streamer_utils.cmake is found
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/src/cmake/mjpg_streamer_utils.cmake)
    message(FATAL_ERROR "mjpg_streamer_utils.cmake not found at ${CMAKE_SOURCE_DIR}/src/cmake/mjpg_streamer_utils.cmake")
endif()

project("mjpg-streamer" C)

# If the user doesn't manually specify a build type, use 'Release'
message("CMAKE_BUILD_TYPE = ${CMAKE_BUILD_TYPE}")
if("${CMAKE_BUILD_TYPE}" STREQUAL "")
  SET(CMAKE_BUILD_TYPE "Release")
endif()

# Suppress CMake developer warnings
set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS ON)

# Suppress specific CMake warnings
set(CMAKE_FIND_PACKAGE_WARN_NO_MODULE ON)
set(CMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY ON)

# Set compiler flags for clean output and fast compilation
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # Release build: minimal warnings, focus on errors only, fast compilation
    SET(COMPILE_DEFINITIONS -Wall -Wno-unused-result -Wno-unused-variable -Wno-unused-function -Wno-format-overflow -Wno-constant-conversion -pipe)
else()
    # Debug build: more warnings for development, fast compilation
    SET(COMPILE_DEFINITIONS -Wall -Werror -pipe)
endif()

# Fast compilation flags for Pi Zero
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pipe -fno-stack-protector")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pipe -fno-stack-protector")

include(CheckLibraryExists)
include(CheckIncludeFiles)
include(FeatureSummary)

# Add include directories for turbojpeg
include_directories(/opt/homebrew/include)

include(mjpg_streamer_utils)

#
# Get the current git hash
#
execute_process(
  COMMAND git rev-parse HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  RESULT_VARIABLE GIT_RESULT
  OUTPUT_VARIABLE GIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(GIT_RESULT EQUAL 0)
  add_definitions("-DGIT_HASH=\"${GIT_HASH}\"")
endif()

#
# Options
#
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -DDEBUG")

add_feature_option(WXP_COMPAT "Enable compatibility with WebcamXP" OFF)

if (WXP_COMPAT)
    add_definitions(-DWXP_COMPAT)
endif (WXP_COMPAT)

set (MJPG_STREAMER_PLUGIN_INSTALL_PATH "lib/mjpg-streamer")

#
# Global dependencies
#

# TurboJPEG is REQUIRED - no fallback to libjpeg
find_library(TURBOJPEG_LIB turbojpeg)
if(TURBOJPEG_LIB)
    set(TURBOJPEG_FOUND TRUE)
    set(JPEG_LIBRARY ${TURBOJPEG_LIB})
    add_definitions(-DHAVE_TURBOJPEG)
    message(STATUS "Found TurboJPEG: ${TURBOJPEG_LIB}")
else()
    message(FATAL_ERROR "TurboJPEG library not found. TurboJPEG is REQUIRED for this project. Please install libturbojpeg-dev (on Linux) or turbojpeg (on macOS via Homebrew).")
endif()


#
# Input plugins
#

add_subdirectory(src/plugins/input_file)
add_subdirectory(src/plugins/input_http)
add_subdirectory(src/plugins/input_raspicam)
add_subdirectory(src/plugins/input_uvc)
add_subdirectory(src/plugins/input_avf)

#
# Output plugins
#

add_subdirectory(src/plugins/output_file)
add_subdirectory(src/plugins/output_http)
add_subdirectory(src/plugins/output_motion)
add_subdirectory(src/plugins/output_rtsp)
add_subdirectory(src/plugins/output_udp)
add_subdirectory(src/plugins/output_viewer)
add_subdirectory(src/plugins/output_viewer_osx)

#
# mjpg_streamer executable
#

# This adds the plugin installation directory to the default DT_RUNPATH, so
# that the user shouldn't need to set LD_LIBRARY_PATH if using 'make install'
# ... however, DT_RUNPATH allows overriding via LD_LIBRARY_PATH if you really
#     need to do it

# Only use --enable-new-dtags on Linux
if(UNIX AND NOT APPLE)
    set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--enable-new-dtags")
endif()
set (CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set (CMAKE_INSTALL_RPATH ${CMAKE_INSTALL_PREFIX}/${MJPG_STREAMER_PLUGIN_INSTALL_PATH})
set (CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)


# Create static library from utils.c and jpeg_utils.c for plugins
add_library(mjpg_streamer_utils STATIC src/utils.c src/jpeg_utils.c)
target_link_libraries(mjpg_streamer_utils ${JPEG_LIBRARY})

add_executable(mjpg_streamer src/mjpg_streamer.c)

target_link_libraries(mjpg_streamer mjpg_streamer_utils pthread dl ${JPEG_LIBRARY})
# Export symbols for dynamic loading
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(mjpg_streamer PRIVATE -Wl,--export-dynamic)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_options(mjpg_streamer PRIVATE -Wl,-export_dynamic)
endif()
install(TARGETS mjpg_streamer DESTINATION bin)

#
# www directory
#

install(DIRECTORY src/www DESTINATION share/mjpg-streamer)


#
# Show enabled/disabled features
#

feature_summary(WHAT ALL)

#
# Final warning
#

if("${CMAKE_CURRENT_SOURCE_DIR}" STREQUAL "${CMAKE_CURRENT_BINARY_DIR}")
  message(WARNING "The source directory is the same as binary directory. \"make clean\" may damage the source tree")
endif()
