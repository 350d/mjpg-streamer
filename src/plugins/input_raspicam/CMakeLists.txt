
find_path(RASPI_INCLUDE_DIR
    NAMES bcm_host.h
    PATHS
        /opt/vc/include
        /usr/include
)

find_library(RASPI_MMAL_CORE_LIB NAMES mmal_core PATHS /opt/vc/lib /usr/lib /usr/lib/arm-linux-gnueabihf)
find_library(RASPI_MMAL_UTIL_LIB NAMES mmal_util PATHS /opt/vc/lib /usr/lib /usr/lib/arm-linux-gnueabihf)
find_library(RASPI_MMAL_VC_CLIENT_LIB NAMES mmal_vc_client PATHS /opt/vc/lib /usr/lib /usr/lib/arm-linux-gnueabihf)
find_library(RASPI_VCOS_LIB NAMES vcos PATHS /opt/vc/lib /usr/lib /usr/lib/arm-linux-gnueabihf)
find_library(RASPI_BCM_HOST_LIB NAMES bcm_host PATHS /opt/vc/lib /usr/lib /usr/lib/arm-linux-gnueabihf)

if (RASPI_INCLUDE_DIR AND RASPI_MMAL_CORE_LIB AND RASPI_MMAL_UTIL_LIB AND
    RASPI_MMAL_VC_CLIENT_LIB AND RASPI_VCOS_LIB AND RASPI_BCM_HOST_LIB)
    set(HAS_RASPI ON)
else()
    set(HAS_RASPI OFF)
endif()

MJPG_STREAMER_PLUGIN_OPTION(input_raspicam "Raspberry Pi input camera plugin"
                            ONLYIF HAS_RASPI)

if (PLUGIN_INPUT_RASPICAM)

    include_directories(${RASPI_INCLUDE_DIR})
    include_directories(${RASPI_INCLUDE_DIR}/interface/vcos)
    include_directories(${RASPI_INCLUDE_DIR}/interface/vcos/pthreads)
    include_directories(${RASPI_INCLUDE_DIR}/interface/vmcs_host)
    include_directories(${RASPI_INCLUDE_DIR}/interface/vmcs_host/linux)

    MJPG_STREAMER_PLUGIN_COMPILE(input_raspicam input_raspicam.c)

    target_link_libraries(input_raspicam
        ${RASPI_MMAL_CORE_LIB}
        ${RASPI_MMAL_UTIL_LIB}
        ${RASPI_MMAL_VC_CLIENT_LIB}
        ${RASPI_VCOS_LIB}
        ${RASPI_BCM_HOST_LIB}
    )

endif()
