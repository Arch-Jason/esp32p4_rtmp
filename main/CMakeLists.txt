set(srcs "main.c"
        "example_buffer.c"
        "example_awb.c"
        "example_pipelines.c"
        "camera.h"
        "camera.c"
        "wifi.c"
        "wifi.h"
        "tcp.c"
        "tcp.h"
        "ov5647_config.c"
        "libflv/flv-writer.h"
        "libflv/flv-writer.c"
        "libflv/flv-muxer.h"
        "libflv/flv-muxer.c"
        "libflv/mpeg4-avc.h"
        "libflv/mpeg4-avc.c"
        "libflv/mpeg4-annexbtomp4.c"
        "libflv/flv-header.c"
)

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
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
)
