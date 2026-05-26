#pragma once

#include <stddef.h>
#include "librtmp/rtmp-client.h"
#include "libflv/flv-muxer.h"

void tcp_server_task(void* pvParameters);
int rtmp_client_send(void* param, const void* header, size_t len, const void* data, size_t bytes);
void rtmp_init();

extern rtmp_client_t* g_rtmp;
extern volatile bool rtmp_ready;
extern flv_muxer_t* flv_muxer;
extern SemaphoreHandle_t rtmp_mutex;