set(PLUGIN_OUTPUT_VIEWER_OSX_AVAILABLE ${APPLE})

MJPG_STREAMER_PLUGIN_OPTION(output_viewer_osx "macOS native output viewer plugin"
                            ONLYIF APPLE)

if (PLUGIN_OUTPUT_VIEWER_OSX)
    MJPG_STREAMER_PLUGIN_COMPILE(output_viewer_osx output_viewer_osx.m)
    target_link_libraries(output_viewer_osx mjpg_streamer_utils)

    add_executable(output_viewer_osx_helper
        AppDelegate.m
        main.m
    )

    set_target_properties(output_viewer_osx_helper PROPERTIES
        OUTPUT_NAME OSXViewer
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src/plugins/output_viewer_osx"
    )

    target_include_directories(output_viewer_osx_helper PRIVATE
        .
    )

    target_link_libraries(output_viewer_osx_helper "-framework Cocoa")
endif()

