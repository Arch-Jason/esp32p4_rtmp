#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "libflv/flv-proto.h"
#include "librtmp/rtmp-client.h"
#include "libflv/flv-muxer.h"

#define RTMP_SERVER_HOST CONFIG_RTMP_SERVER_HOST
#define RTMP_SERVER_PORT CONFIG_RTMP_SERVER_PORT 
#define RTMP_APP         CONFIG_RTMP_APP
#define RTMP_STREAM      CONFIG_RTMP_STREAM

static const char* TAG = "RTMP_CLIENT";

rtmp_client_t* g_rtmp = NULL;
volatile bool rtmp_ready = false;
volatile int rtmp_sock = -1;

flv_muxer_t* flv_muxer;
static struct rtmp_client_handler_t handler;
SemaphoreHandle_t rtmp_mutex = NULL;

#define RTMP_QUEUE_SIZE 64
typedef struct {
    uint8_t *buf;
    size_t len;
} rtmp_msg_t;

QueueHandle_t rtmp_queue = NULL;

static int64_t start_stream_time = 0;

uint32_t get_stream_timestamp(void) {
    if (start_stream_time == 0) {
        start_stream_time = esp_timer_get_time() / 1000;
    }
    return (uint32_t)(esp_timer_get_time() / 1000 - start_stream_time);
}

void reset_stream_timestamp(void) {
    start_stream_time = esp_timer_get_time() / 1000;
}


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

void rtmp_sender_task(void* pvParameters) {
    rtmp_msg_t msg;
    while (1) {
        if (xQueueReceive(rtmp_queue, &msg, portMAX_DELAY)) {
            if (rtmp_sock >= 0) {
                size_t sent = 0;
                while (sent < msg.len) {
                    int ret = send(rtmp_sock, (const char*)msg.buf + sent, msg.len - sent, 0);
                    if (ret <= 0) {
                        ESP_LOGE(TAG, "Async Send failed! ret=%d, errno=%d", ret, errno);
                        break; 
                    }
                    sent += ret;
                }
                // ESP_LOGI(TAG, "Async sent %d bytes", (int)sent);
            } else {
                ESP_LOGW(TAG, "Socket not ready, dropping %d bytes", (int)msg.len);
            }
            free(msg.buf); // 释放分配的内存
        }
    }
}

int rtmp_client_send(void* param, const void* header, size_t len, const void* data, size_t bytes) {
    size_t total = len + bytes;
    uint8_t *buffer = (uint8_t*)malloc(total);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to malloc for RTMP packet");
        return -1;
    }

    if (header && len > 0) memcpy(buffer, header, len);
    if (data && bytes > 0) memcpy(buffer + len, data, bytes);

    rtmp_msg_t msg = { .buf = buffer, .len = total };
    // ESP_LOGI(TAG, "Queuing %d bytes", (int)total);

    // 发送队列，如果队列满则阻塞，或者根据需要调整超时
    if (xQueueSend(rtmp_queue, &msg, pdMS_TO_TICKS(1000)) != pdPASS) {
        ESP_LOGE(TAG, "RTMP queue full, dropping packet");
        free(buffer);
        return -1;
    }
    
    return (int)total;
}

void tcp_server_task(void* pvParameters) {
    char rx_buffer[2048];
    struct addrinfo hints;
    struct addrinfo *res;

    while (1) {
        rtmp_ready = false;
        ESP_LOGI(TAG, "Resolving and connecting to RTMP Server: %s:%d", RTMP_SERVER_HOST, RTMP_SERVER_PORT);

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        char port_str[6];
        snprintf(port_str, sizeof(port_str), "%d", RTMP_SERVER_PORT);

        int dns_err = getaddrinfo(RTMP_SERVER_HOST, port_str, &hints, &res);
        if (dns_err != 0 || res == NULL) {
            ESP_LOGE(TAG, "DNS lookup failed for %s, err=%d", RTMP_SERVER_HOST, dns_err);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        rtmp_sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (rtmp_sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            freeaddrinfo(res);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        int err = connect(rtmp_sock, res->ai_addr, res->ai_addrlen);
        freeaddrinfo(res);
        
        if (err != 0) {
            ESP_LOGE(TAG, "Socket connect failed: errno %d", errno);
            close(rtmp_sock);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "TCP Connected! Starting RTMP Handshake...");
        xQueueReset(rtmp_queue);

        int yes = 1;
        setsockopt(rtmp_sock, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));

        char tcurl[256];
        snprintf(tcurl, sizeof(tcurl), "rtmp://%s/%s", RTMP_SERVER_HOST, RTMP_APP);

        g_rtmp = rtmp_client_create(RTMP_APP, RTMP_STREAM, tcurl, &rtmp_sock, &handler);
        if (!g_rtmp) {
            ESP_LOGE(TAG, "Failed to create rtmp client instance");
            close(rtmp_sock);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // 执行并捕获具体的错误返回值
        int start_res = rtmp_client_start(g_rtmp, 0);
        if (0 != start_res) {
            ESP_LOGE(TAG, "Failed to start rtmp client, error code: %d", start_res);
            rtmp_client_destroy(g_rtmp);
            g_rtmp = NULL;
            close(rtmp_sock);
            rtmp_sock = -1;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // 网络事件轮询接收循环
        while (1) {
            int len = recv(rtmp_sock, rx_buffer, sizeof(rx_buffer), 0);
            // if (len > 0) ESP_LOGI(TAG, "Recv %d bytes", len);
            if (len < 0) {
                ESP_LOGE(TAG, "Recv failed: errno %d", errno);
                break;
            } else if (len == 0) {
                ESP_LOGI(TAG, "Connection closed by server");
                break;
            }

            int r = rtmp_client_input(g_rtmp, rx_buffer, len);
            if (r != 0) {
                ESP_LOGE(TAG, "rtmp_client_input error: %d", r);
                break;
            }

            int state = rtmp_client_getstate(g_rtmp);
            if (state == 4) { 
                if (!rtmp_ready) {
                    ESP_LOGI(TAG, "RTMP Handshake & Protocol complete! Ready to stream.");
                    reset_stream_timestamp();
                    rtmp_ready = true;
                }
            } else {
                rtmp_ready = false;
            }
        }

        ESP_LOGW(TAG, "RTMP disconnected. Cleaning up and reconnecting...");
        rtmp_ready = false;
        xQueueReset(rtmp_queue);
        if (g_rtmp) {
            rtmp_client_destroy(g_rtmp);
            g_rtmp = NULL;
        }
        close(rtmp_sock);
        rtmp_sock = -1;
        vTaskDelay(pdMS_TO_TICKS(2000)); 
    }
}

void rtmp_init() {
    rtmp_mutex = xSemaphoreCreateBinary();
    xSemaphoreGive(rtmp_mutex);
    rtmp_queue = xQueueCreate(RTMP_QUEUE_SIZE, sizeof(rtmp_msg_t));
    xTaskCreate(rtmp_sender_task, "rtmp_sender", 10240, NULL, 10, NULL);
    memset(&handler, 0, sizeof(handler));
    handler.send = rtmp_client_send;
    flv_muxer = flv_muxer_create(on_flv_packet, NULL);
}