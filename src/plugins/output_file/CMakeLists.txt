
MJPG_STREAMER_PLUGIN_OPTION(output_file "File output plugin")
MJPG_STREAMER_PLUGIN_COMPILE(output_file output_file.c)
target_link_libraries(output_file mjpg_streamer_utils)

