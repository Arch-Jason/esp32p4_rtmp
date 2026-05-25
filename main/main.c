#include "esp_cache.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera.h"
#include "rtmp_tcp.h"
#include "wifi.h"

static const char* TAG = "CameraMain";

void app_main(void) {
    wifi_init_sta();
    while (!wifi_connect())
        vTaskDelay(100 / portTICK_PERIOD_MS);

    camera_init();
    rtmp_init();

    xTaskCreate(tcp_server_task, "rtmp_client_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Starting camera frame acquisition loop");

    // 主循环：获取采集到的 H264 NALU 并推流
    while (1) {
        char* buf = NULL;
        size_t len = 0;
        get_h264_nalu(&buf, &len, (uint32_t*) &dts, (uint32_t*) &pts);

        // 只有当 RTMP 握手成功之后，才把 H264 发送给混流器
        if (rtmp_ready && g_rtmp != NULL && len != 0) {
            flv_muxer_avc(flv_muxer, (const uint8_t*) buf, len, pts, dts);
            // ESP_LOGI(TAG, "DTS: %u, PTS: %u\r\n", dts, pts);
        }

        vTaskDelay(1);
    }
}