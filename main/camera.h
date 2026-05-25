#pragma once
#include <stddef.h>
#include <stdint.h>

void camera_init();
void get_h264_nalu(char** buf, size_t* len, uint32_t* dts, uint32_t* pts);

extern volatile uint32_t dts, pts;