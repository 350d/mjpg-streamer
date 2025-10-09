cmake_minimum_required(VERSION 3.10)

# Motion detection output plugin
add_library(output_motion SHARED output_motion.c)

target_include_directories(output_motion PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

# Find JPEG library (check environment variables first for cross-compilation)
if(JPEG_INCLUDE_DIR AND JPEG_LIBRARY)
    # Use provided JPEG paths for cross-compilation
    target_include_directories(output_motion PRIVATE ${JPEG_INCLUDE_DIR})
    target_link_libraries(output_motion ${JPEG_LIBRARY})
    target_compile_definitions(output_motion PRIVATE HAVE_JPEG)
    message(STATUS "Using provided JPEG: ${JPEG_LIBRARY}")
else()
    # Find JPEG library using standard methods
    find_library(JPEG_LIBRARY jpeg)
    find_path(JPEG_INCLUDE_DIR jpeglib.h)
    if(JPEG_LIBRARY AND JPEG_INCLUDE_DIR)
        target_include_directories(output_motion PRIVATE ${JPEG_INCLUDE_DIR})
        target_link_libraries(output_motion ${JPEG_LIBRARY})
        target_compile_definitions(output_motion PRIVATE HAVE_JPEG)
    else()
        message(WARNING "libjpeg not found - motion detection will be disabled")
        target_compile_definitions(output_motion PRIVATE NO_JPEG)
    endif()
endif()

# Find CURL library (check environment variables first for cross-compilation)
if(CURL_INCLUDE_DIR AND CURL_LIBRARY)
    # Use provided CURL paths for cross-compilation
    target_include_directories(output_motion PRIVATE ${CURL_INCLUDE_DIR})
    target_link_libraries(output_motion ${CURL_LIBRARY})
    target_compile_definitions(output_motion PRIVATE HAVE_CURL)
    message(STATUS "Using provided CURL: ${CURL_LIBRARY}")
else()
    # Find CURL library using pkg-config
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(CURL libcurl)
        if(CURL_FOUND)
            target_include_directories(output_motion PRIVATE ${CURL_INCLUDE_DIRS})
            target_link_libraries(output_motion ${CURL_LIBRARIES})
            target_compile_definitions(output_motion PRIVATE HAVE_CURL)
        else()
            message(WARNING "libcurl not found - webhook functionality will be disabled")
            target_compile_definitions(output_motion PRIVATE NO_CURL)
        endif()
    else()
        message(WARNING "pkg-config not found - webhook functionality will be disabled")
        target_compile_definitions(output_motion PRIVATE NO_CURL)
    endif()
endif()

# mbedTLS support removed - HTTPS webhooks disabled

target_link_libraries(output_motion
    ${CMAKE_THREAD_LIBS_INIT}
)

# Set output directory
set_target_properties(output_motion PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
    PREFIX ""
)
