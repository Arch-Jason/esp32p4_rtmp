#include "esp_cache.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera.h"
#include "tcp.h"
#include "wifi.h"

#include "libflv/flv-muxer.h"
#include "libflv/flv-proto.h"
#include "librtmp/rtmp-client.h"

static const char* TAG = "CameraMain";

volatile uint32_t dts, pts;

// 引用来自 tcp.c 的全局变量
extern rtmp_client_t* g_rtmp;
extern volatile bool rtmp_ready;

static int on_flv_packet(void* flv, int type, const void* data, size_t bytes, uint32_t timestamp) {
    // 如果 RTMP 还没有握手建立成功，直接丢弃帧，防止阻塞
    if (!rtmp_ready || g_rtmp == NULL) {
        return 0;
    }

    int r = 0;
    if (type == FLV_TYPE_VIDEO) {
        // 直接调用库函数将 FLV 视频负载推入 RTMP 管道
        r = rtmp_client_push_video(g_rtmp, data, bytes, timestamp);
    } else if (type == FLV_TYPE_AUDIO) {
        r = rtmp_client_push_audio(g_rtmp, data, bytes, timestamp);
    } else if (type == FLV_TYPE_SCRIPT) {
        r = rtmp_client_push_script(g_rtmp, data, bytes, timestamp);
    }

    if (r != 0) {
        ESP_LOGE(TAG, "RTMP push packet failed, error code: %d", r);
    }
    return r;
}

void app_main(void) {
    wifi_init_sta();
    while (!wifi_connect())
        vTaskDelay(100 / portTICK_PERIOD_MS);

    xTaskCreate(tcp_server_task, "rtmp_client_task", 4096, NULL, 5, NULL);

    camera_init();

    // 分配外部 SPIRAM 缓存区
    void* flv_buf = heap_caps_aligned_alloc(64, 3 * 1024 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!flv_buf) {
        ESP_LOGE(TAG, "Failed to allocate SPIRAM buffer");
        return;
    }

    ESP_LOGI(TAG, "Starting camera frame acquisition loop");

    // 创建 FLV 混流器，当一帧 H264 转换为 FLV 格式后会回调 on_flv_packet
    flv_muxer_t* flv_muxer = flv_muxer_create(on_flv_packet, NULL);

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