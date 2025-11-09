
add_definitions(-D_GNU_SOURCE)

MJPG_STREAMER_PLUGIN_OPTION(output_http "HTTP server output plugin")
MJPG_STREAMER_PLUGIN_COMPILE(output_http httpd.c output_http.c)
target_link_libraries(output_http mjpg_streamer_utils)
