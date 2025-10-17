
# Check for inotify headers (skip check for cross-compilation)
if(CMAKE_CROSSCOMPILING)
    # For cross-compilation, assume inotify is available
    set(HAVE_SYS_INOTIFY_H TRUE)
    message(STATUS "Cross-compilation detected - assuming inotify support")
else()
    check_include_files(sys/inotify.h HAVE_SYS_INOTIFY_H)
endif()

MJPG_STREAMER_PLUGIN_OPTION(input_file "File input plugin" ONLYIF HAVE_SYS_INOTIFY_H)
MJPG_STREAMER_PLUGIN_COMPILE(input_file input_file.c)
if(TARGET input_file)
    target_link_libraries(input_file mjpg_streamer_utils)
endif()


