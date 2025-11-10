MJPG_STREAMER_PLUGIN_OPTION(output_motion "Motion detection output plugin")

if (PLUGIN_OUTPUT_MOTION)
    MJPG_STREAMER_PLUGIN_COMPILE(output_motion output_motion.c)

    target_include_directories(output_motion PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../..
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    )

    set(_output_motion_link_libs mjpg_streamer_utils ${CMAKE_THREAD_LIBS_INIT})

    # Use the same JPEG library as the main project (TurboJPEG or libjpeg)
    if(JPEG_LIBRARY)
        list(APPEND _output_motion_link_libs ${JPEG_LIBRARY})
        target_compile_definitions(output_motion PRIVATE HAVE_JPEG)
        if(JPEG_INCLUDE_DIR)
            target_include_directories(output_motion PRIVATE ${JPEG_INCLUDE_DIR})
        endif()
        # Add TurboJPEG include path if available
        if(TURBOJPEG_FOUND)
            target_include_directories(output_motion PRIVATE /opt/homebrew/include)
            target_compile_definitions(output_motion PRIVATE HAVE_TURBOJPEG)
        endif()
        message(STATUS "Using JPEG library for output_motion: ${JPEG_LIBRARY}")
    else()
        message(WARNING "JPEG library not found - motion detection will be disabled")
        target_compile_definitions(output_motion PRIVATE NO_JPEG)
    endif()

    # Find CURL library (check environment variables first for cross-compilation)
    if(CURL_INCLUDE_DIR AND CURL_LIBRARY)
        # Use provided CURL paths for cross-compilation
        target_include_directories(output_motion PRIVATE ${CURL_INCLUDE_DIR})
        list(APPEND _output_motion_link_libs ${CURL_LIBRARY})
        target_compile_definitions(output_motion PRIVATE HAVE_CURL)
        message(STATUS "Using provided CURL: ${CURL_LIBRARY}")
    else()
        # Find CURL library using pkg-config
        find_package(PkgConfig)
        if(PkgConfig_FOUND)
            pkg_check_modules(CURL libcurl)
            if(CURL_FOUND)
                target_include_directories(output_motion PRIVATE ${CURL_INCLUDE_DIRS})
                list(APPEND _output_motion_link_libs ${CURL_LIBRARIES})
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

    target_link_libraries(output_motion ${_output_motion_link_libs})
endif()
