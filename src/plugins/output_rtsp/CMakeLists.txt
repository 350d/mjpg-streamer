
MJPG_STREAMER_PLUGIN_OPTION(output_rtsp "RTSP Server output plugin")
include_directories(/opt/homebrew/include)
MJPG_STREAMER_PLUGIN_COMPILE(output_rtsp output_rtsp.c)
