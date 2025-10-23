# Check for macOS and AVFoundation
if(APPLE)
    set(HAVE_AVFOUNDATION TRUE)
    message(STATUS "macOS detected - enabling AVFoundation support")
else()
    set(HAVE_AVFOUNDATION FALSE)
    message(STATUS "Non-macOS system - AVFoundation not available")
endif()

MJPG_STREAMER_PLUGIN_OPTION(input_avf "macOS AVFoundation input plugin"
                            ONLYIF HAVE_AVFOUNDATION)

if (PLUGIN_INPUT_AVF)
    
    # macOS specific settings
    add_definitions(-DMACOS -D_AVFOUNDATION_AVAILABLE)
    
    # Find required frameworks
    find_library(AVFOUNDATION_LIB AVFoundation)
    find_library(COREMEDIA_LIB CoreMedia)
    find_library(COREVIDEO_LIB CoreVideo)
    find_library(FOUNDATION_LIB Foundation)
    
    if(AVFOUNDATION_LIB AND COREMEDIA_LIB AND COREVIDEO_LIB AND FOUNDATION_LIB)
        message(STATUS "Found required macOS frameworks")
    else()
        message(FATAL_ERROR "Required macOS frameworks not found")
    endif()
    
    # Use the same JPEG library as the main project (TurboJPEG or libjpeg)
    if(JPEG_LIBRARY)
        set(JPEG_LIB ${JPEG_LIBRARY})
        set(JPEG_FOUND TRUE)
        message(STATUS "Using JPEG library for input_avf: ${JPEG_LIBRARY}")
    else()
        add_definitions(-DNO_LIBJPEG)
        message(WARNING "JPEG library not found - JPEG compression will be disabled")
    endif()

    MJPG_STREAMER_PLUGIN_COMPILE(input_avf input_avf.m)
    
    target_link_libraries(input_avf mjpg_streamer_utils)
    
    # Link macOS frameworks
    target_link_libraries(input_avf 
        ${AVFOUNDATION_LIB}
        ${COREMEDIA_LIB}
        ${COREVIDEO_LIB}
        ${FOUNDATION_LIB}
    )
    
    # Link JPEG library if available
    if (JPEG_LIBRARY)
        target_link_libraries(input_avf ${JPEG_LIBRARY})
        if(JPEG_INCLUDE_DIR)
            target_include_directories(input_avf PRIVATE ${JPEG_INCLUDE_DIR})
        endif()
    endif (JPEG_LIBRARY)
    
    # Set Objective-C compilation flags
    set_target_properties(input_avf PROPERTIES
        COMPILE_FLAGS "-fobjc-arc -fobjc-link-runtime"
        LINK_FLAGS "-fobjc-arc"
    )
    
endif()
