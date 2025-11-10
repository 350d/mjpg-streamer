MJPG_STREAMER_PLUGIN_OPTION(output_viewer_osx "macOS native output viewer plugin"
                            ONLYIF APPLE)

if (PLUGIN_OUTPUT_VIEWER_OSX)
    MJPG_STREAMER_PLUGIN_COMPILE(output_viewer_osx output_viewer_osx.m)
    target_link_libraries(output_viewer_osx mjpg_streamer_utils)

    add_executable(output_viewer_osx_helper
        ../output_viewer_osx_app/main.m
        ../output_viewer_osx_app/AppDelegate.m
    )

    set_target_properties(output_viewer_osx_helper PROPERTIES
        OUTPUT_NAME OSXViewer
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src/plugins/output_viewer_osx_app"
    )

    target_include_directories(output_viewer_osx_helper PRIVATE
        ../output_viewer_osx_app
    )

    target_link_libraries(output_viewer_osx_helper "-framework Cocoa")
endif()

