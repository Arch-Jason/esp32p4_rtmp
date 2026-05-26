#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"

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
int rtmp_sock = -1;

flv_muxer_t* flv_muxer;
static struct rtmp_client_handler_t handler;
SemaphoreHandle_t rtmp_mutex = NULL;

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

int rtmp_client_send(void* param, const void* header, size_t len, const void* data, size_t bytes) {
    int socket_fd = *(int*)param;
    if (socket_fd < 0) {
        return -1;
    }

    // 1. 发送头部
    if (len > 0) {
        size_t sent = 0;
        while (sent < len) {
            int ret = send(socket_fd, (const char*)header + sent, len - sent, 0);
            if (ret <= 0) {
                ESP_LOGE(TAG, "Send header failed! ret=%d, errno=%d", ret, errno);
                return -1;
            }
            sent += ret;
        }
    }
    // 2. 发送主体数据
    if (bytes > 0) {
        size_t sent = 0;
        while (sent < bytes) {
            int ret = send(socket_fd, (const char*)data + sent, bytes - sent, 0);
            if (ret <= 0) {
                ESP_LOGE(TAG, "Send data failed! ret=%d, errno=%d", ret, errno);
                return -1;
            }
            sent += ret;
        }
    }
    
    return (int)(len + bytes);
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
                    rtmp_ready = true;
                }
            } else {
                rtmp_ready = false;
            }
        }

        ESP_LOGW(TAG, "RTMP disconnected. Cleaning up and reconnecting...");
        rtmp_ready = false;
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
    memset(&handler, 0, sizeof(handler));
    handler.send = rtmp_client_send;
    flv_muxer = flv_muxer_create(on_flv_packet, NULL);
}