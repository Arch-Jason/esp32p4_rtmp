file(GLOB_RECURSE FDKAAC_SOURCES
    "fdk-aac/*.cpp"
    "fdk-aac/*.c"
)

# 排除平台优化代码
list(FILTER FDKAAC_SOURCES EXCLUDE REGEX ".*/mips/.*")
list(FILTER FDKAAC_SOURCES EXCLUDE REGEX ".*/arm/.*")
list(FILTER FDKAAC_SOURCES EXCLUDE REGEX ".*/x86/.*")
list(FILTER FDKAAC_SOURCES EXCLUDE REGEX ".*\\.S$")

set(srcs "main.c"
        "example_buffer.c"
        "example_awb.c"
        "example_pipelines.c"
        "camera.c"
        "wifi.c"
        "rtmp_tcp.c"
        "i2s_mic.c"
       
        "libflv/flv-writer.h"
        "libflv/flv-writer.c"
        "libflv/flv-muxer.h"
        "libflv/flv-muxer.c"
        "libflv/mpeg4-avc.h"
        "libflv/mpeg4-avc.c"
        "libflv/mpeg4-annexbtomp4.c"
        "libflv/flv-header.c"
        "libflv/mpeg4-aac.c"
        "libflv/mpeg4-aac-asc.c"
        "librtmp/rtmp-client.c"
        "librtmp/rtmp-internal.h"
        "librtmp/rtmp-chunk-write.c"
        "librtmp/rtmp-chunk-read.c"
        "librtmp/rtmp-chunk-header.c"
        "librtmp/rtmp-handshake.c"
        "librtmp/rtmp-handler.c"
        "librtmp/rtmp-invoke-handler.c"
        "librtmp/rtmp-control-handler.c"
        "librtmp/rtmp-control-message.c"
        "librtmp/rtmp-netconnection.c"
        "librtmp/rtmp-event.c"
        "librtmp/rtmp-netstream.c"
        "libflv/amf0.c"

        ${FDKAAC_SOURCES}
)

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS
                       "."
                       "fdk-aac/libSYS/include"
                       "fdk-aac/libAACenc/include"
                       "fdk-aac/libFDK/include"
                       "fdk-aac/libMpegTPEnc/include"
                       "fdk-aac/libSBRenc/include"
                       "fdk-aac/libPCMutils/include"
                       "fdk-aac/libSACenc/include"
        REQUIRES
                esp_mm
                esp_driver_isp
                esp_driver_cam
                esp_driver_i2c
                sensor_init
                esp_lcd
                esp_wifi
                nvs_flash
                esp_h264
                esp_timer
                esp_driver_i2s
                esp_driver_gpio
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -ffunction-sections
    -fdata-sections
    -Wno-parentheses
    -Wno-unused-variable
    -Wno-sign-compare
    -Wno-error
    -Wno-incompatible-pointer-types
)

target_link_options(${COMPONENT_LIB} PRIVATE
    -Wl,--gc-sections
)