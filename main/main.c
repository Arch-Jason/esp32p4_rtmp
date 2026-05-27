#include "esp_cache.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera.h"
#include "i2s_mic.h"
#include "rtmp_tcp.h"
#include "wifi.h"

static const char* TAG = "CameraMain";

void video_task(void* pvParameters) {
    ESP_LOGI(TAG, "Starting camera frame acquisition loop");

    while (1) {
        char* buf = NULL;
        size_t len = 0;
        uint32_t dts = 0, pts = 0;
        get_h264_nalu(&buf, &len, &dts, &pts);

        if (rtmp_ready && g_rtmp != NULL && len != 0) {
            if (xSemaphoreTake(rtmp_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                flv_muxer_avc(flv_muxer, (const uint8_t*) buf, len, pts, dts);
                xSemaphoreGive(rtmp_mutex);
            } else {
                ESP_LOGW(TAG, "Missed frame due to lock timeout");
            }
        }
    }
}

void app_main(void) {
    wifi_init_sta();
    while (!wifi_connect())
        vTaskDelay(100 / portTICK_PERIOD_MS);

    camera_init();
    rtmp_init();

    xTaskCreate(tcp_server_task, "rtmp_client_task", 4096, NULL, 5, NULL);

    if (!i2s_mic_init())
        ESP_LOGW(TAG, "Mic init failed");

    xTaskCreatePinnedToCore(video_task, "video_task", 8192, NULL, 5, NULL, 0);
}