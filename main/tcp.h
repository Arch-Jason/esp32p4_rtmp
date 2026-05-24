#pragma once

#include "lwip/sockets.h"

extern volatile bool flv_header_sent;
extern int sock;
void tcp_server_task(void *pvParameters);
void tcp_tx(char* buf, size_t len);