
# Check for Linux V4L2 headers (skip check for cross-compilation)
if(CMAKE_CROSSCOMPILING)
    # For cross-compilation, assume V4L2 is available
    set(HAVE_LINUX_VIDEODEV2_H TRUE)
    message(STATUS "Cross-compilation detected - assuming V4L2 support")
else()
    check_include_files(linux/videodev2.h HAVE_LINUX_VIDEODEV2_H)
endif()

MJPG_STREAMER_PLUGIN_OPTION(input_uvc "Video 4 Linux input plugin"
                            ONLYIF HAVE_LINUX_VIDEODEV2_H)

if (PLUGIN_INPUT_UVC)
    
    add_definitions(-DLINUX -D_GNU_SOURCE)
    
    # For cross-compilation, skip V4L2 library check
    if(NOT CMAKE_CROSSCOMPILING)
        find_library(V4L2_LIB v4l2)
        if (V4L2_LIB)
            add_definitions(-DUSE_LIBV4L2)
        endif (V4L2_LIB)
    else()
        message(STATUS "Cross-compilation: skipping V4L2 library check")
    endif()
    
    # Use the same JPEG library as the main project (TurboJPEG or libjpeg)
    if(JPEG_LIBRARY)
        set(JPEG_LIB ${JPEG_LIBRARY})
        set(JPEG_FOUND TRUE)
        message(STATUS "Using JPEG library for input_uvc: ${JPEG_LIBRARY}")
    else()
        add_definitions(-DNO_LIBJPEG)
        message(WARNING "JPEG library not found - JPEG compression will be disabled")
    endif()

    MJPG_STREAMER_PLUGIN_COMPILE(input_uvc dynctrl.c
                                           input_uvc.c
                                           v4l2uvc.c)
    
    target_link_libraries(input_uvc mjpg_streamer_utils)

    if (V4L2_LIB)
        target_link_libraries(input_uvc ${V4L2_LIB})
    endif (V4L2_LIB)

    if (JPEG_LIBRARY)
        target_link_libraries(input_uvc ${JPEG_LIBRARY})
        if(JPEG_INCLUDE_DIR)
            target_include_directories(input_uvc PRIVATE ${JPEG_INCLUDE_DIR})
        endif()
    endif (JPEG_LIBRARY)

endif()
