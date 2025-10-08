
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
    
    # Find JPEG library with cross-compilation support
    if(JPEG_INCLUDE_DIR AND JPEG_LIBRARY)
        # Use provided JPEG paths for cross-compilation
        set(JPEG_LIB ${JPEG_LIBRARY})
        set(JPEG_FOUND TRUE)
        message(STATUS "Using provided JPEG for input_uvc: ${JPEG_LIBRARY}")
    else()
        # Find JPEG library normally
        find_library(JPEG_LIB jpeg)
        if(JPEG_LIB)
            set(JPEG_FOUND TRUE)
        endif()
    endif()
    
    if (NOT JPEG_LIB)
        add_definitions(-DNO_LIBJPEG)
    endif (NOT JPEG_LIB)

    MJPG_STREAMER_PLUGIN_COMPILE(input_uvc dynctrl.c
                                           input_uvc.c
                                           jpeg_utils.c
                                           v4l2uvc.c)
    
    target_link_libraries(input_uvc mjpg_streamer_utils)

    if (V4L2_LIB)
        target_link_libraries(input_uvc ${V4L2_LIB})
    endif (V4L2_LIB)

    if (JPEG_LIB)
        target_link_libraries(input_uvc ${JPEG_LIB})
        if(JPEG_INCLUDE_DIR)
            target_include_directories(input_uvc PRIVATE ${JPEG_INCLUDE_DIR})
        endif()
    endif (JPEG_LIB)

endif()
