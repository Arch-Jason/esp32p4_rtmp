#include "tcp.h"
#include "esp_log.h"

#define PORT CONFIG_PORT
#define KEEPALIVE_IDLE CONFIG_KEEPALIVE_IDLE
#define KEEPALIVE_INTERVAL CONFIG_KEEPALIVE_INTERVAL
#define KEEPALIVE_COUNT CONFIG_KEEPALIVE_COUNT

static const char* TAG = "TCP";

int sock = -1;
volatile bool flv_header_sent;

void tcp_server_task(void* pvParameters) {
    char addr_str[128];
    int addr_family = (int) pvParameters;
    int ip_protocol = 0;
    int keepAlive = 1;
    int keepIdle = KEEPALIVE_IDLE;
    int keepInterval = KEEPALIVE_INTERVAL;
    int keepCount = KEEPALIVE_COUNT;
    struct sockaddr_storage dest_addr;

    if (addr_family == AF_INET) {
        struct sockaddr_in* dest_addr_ip4 = (struct sockaddr_in*) &dest_addr;
        dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr_ip4->sin_family = AF_INET;
        dest_addr_ip4->sin_port = htons(PORT);
        ip_protocol = IPPROTO_IP;
    }

    int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    ESP_LOGI(TAG, "Socket created");

    int err = bind(listen_sock, (struct sockaddr*) &dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        ESP_LOGE(TAG, "IPPROTO: %d", addr_family);
        goto CLEAN_UP;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", PORT);

    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        goto CLEAN_UP;
    }

    while (1) {
        ESP_LOGI(TAG, "Socket listening");

        struct sockaddr_storage source_addr;
        socklen_t addr_len = sizeof(source_addr);
        sock = accept(listen_sock, (struct sockaddr*) &source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }

        // Set tcp keepalive option
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));
        int yes = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
        // Convert ip address to string
        if (source_addr.ss_family == PF_INET) {
            inet_ntoa_r(((struct sockaddr_in*) &source_addr)->sin_addr,
                        addr_str,
                        sizeof(addr_str) - 1);
        }
        ESP_LOGI(TAG, "Socket accepted ip address: %s", addr_str);

        flv_header_sent = false;
        static const uint8_t flv_header[] = {'F',
                                             'L',
                                             'V',
                                             0x01,
                                             0x01, // video only
                                             0x00,
                                             0x00,
                                             0x00,
                                             0x09,
                                             0x00,
                                             0x00,
                                             0x00,
                                             0x00};

        tcp_tx((char*) flv_header, sizeof(flv_header));

        flv_header_sent = true;

        char rx_buffer[128];
        while (1) {
            int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);

            if (len < 0) {
                ESP_LOGE(TAG, "recv failed: errno %d", errno);
                break;
            } else if (len == 0) {
                ESP_LOGI(TAG, "Connection closed by client");
                break;
            } else {
                // 收到数据，进行处理
                rx_buffer[len] = 0; // 确保字符串以 null 结尾
                ESP_LOGI(TAG, "Received %d bytes: %s", len, rx_buffer);
            }
        }

        flv_header_sent = false;

        sock = -1;
        shutdown(sock, 0);
        close(sock);
    }

CLEAN_UP:
    close(listen_sock);
    vTaskDelete(NULL);
}

void tcp_tx(char* buf, size_t len) {
    int sent = 0;
    while (sent < len) {
        int ret = send(sock, buf + sent, len - sent, 0);
        if (ret <= 0)
            return;
        sent += ret;
    }
}