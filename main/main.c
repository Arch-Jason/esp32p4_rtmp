#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera.h"
#include "tcp.h"
#include "wifi.h"

#include "libflv/flv-muxer.h"
#include "libflv/flv-writer.h"
#include "libflv/mpeg4-avc.h"

#include "libflv/flv-header.h"
#define FLV_TAG_HEADER_SIZE 11 // StreamID included

static const char* TAG = "Camera";

volatile uint32_t dts, pts;

struct h264_raw_t {
    flv_muxer_t* flv;
    uint32_t pts, dts;
    const uint8_t* ptr;
    int vcl;
};

static int on_flv_packet(void* flv, int type, const void* data, size_t bytes, uint32_t timestamp) {
    uint8_t buf[FLV_TAG_HEADER_SIZE + 4];

    struct flv_tag_header_t tag;
    memset(&tag, 0, sizeof(tag));

    tag.size = bytes;
    tag.type = type;
    tag.timestamp = timestamp;

    flv_tag_header_write(&tag, buf, FLV_TAG_HEADER_SIZE);

    flv_tag_size_write(buf + FLV_TAG_HEADER_SIZE, 4, bytes + FLV_TAG_HEADER_SIZE);

    tcp_tx((char*) buf, FLV_TAG_HEADER_SIZE);

    tcp_tx((char*) data, bytes);

    tcp_tx((char*) (buf + FLV_TAG_HEADER_SIZE), 4);

    return 0;
}

void app_main(void) {
    wifi_init_sta();
    while (!wifi_connect())
        vTaskDelay(100 / portTICK_PERIOD_MS);

    xTaskCreate(tcp_server_task, "tcp_server", 4096, (void*) AF_INET, 5, NULL);

    camera_init();
    void* flv_buf = heap_caps_aligned_alloc(64, 3 * 1024 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);

    ESP_LOGI(TAG, "Camera loop");

    flv_muxer_t* flv_muxer = flv_muxer_create(on_flv_packet, NULL);

    // Main loop: receive frames
    while (1) {
        char* buf = NULL;
        size_t len = 0;

        get_h264_nalu(&buf, &len, (uint32_t*) &dts, (uint32_t*) &pts);

        if (buf && len > 0) {
            if (sock >= 0 && flv_header_sent) {
                flv_muxer_avc(flv_muxer, (const uint8_t*) buf, len, pts, dts);
            }
        }

        vTaskDelay(1);
    }
}
