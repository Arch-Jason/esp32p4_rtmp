#include "camera.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_ctlr_types.h"
#include "example_buffer.h"
#include "example_config.h"
#include "example_pipelines.h"
#include "freertos/semphr.h"
#include <driver/isp_core.h>
#include <driver/isp_types.h>
#include <esp_cache.h>
#include <esp_cam_ctlr_csi.h>
#include <esp_h264_alloc.h>
#include <esp_h264_enc_single_hw.h>
#include <esp_ldo_regulator.h>
#include <esp_timer.h>
#include <example_sensor_init.h>
#include <stdint.h>
#include "rtmp_tcp.h"

static const char* TAG = "Camera utils";

volatile uint32_t frames_received = 0;
volatile void* current_frame = NULL;
SemaphoreHandle_t frame_sync_sem = NULL;
volatile bool encoder_busy = false;


/**
 * @brief Camera callback: Get new video buffer
 *
 * This callback is called when CSI needs a new buffer to write frame data.
 */
bool s_camera_get_new_vb(esp_cam_ctlr_handle_t handle,
                         esp_cam_ctlr_trans_t* trans,
                         void* user_data) {
    example_pingpong_buffer_ctx_t* ctx = (example_pingpong_buffer_ctx_t*) user_data;

    // Provide the current CSI buffer for the next frame
    trans->buffer = example_isp_buffer_get_csi_buffer(ctx);
    trans->buflen = CONFIG_CAM_H * CONFIG_CAM_V * 3 / 2;

    return false;
}

/**
 * @brief Camera callback: Frame transfer finished
 *
 * This callback is called when CSI finishes writing a frame.
 * It handles buffer swapping and display update.
 */
bool s_camera_get_finished_trans(esp_cam_ctlr_handle_t handle,
                                 esp_cam_ctlr_trans_t* trans,
                                 void* user_data) {
    example_pingpong_buffer_ctx_t* ctx = (example_pingpong_buffer_ctx_t*) user_data;

    frames_received++;

    // 尝试获取锁（非阻塞，允许在 ISR/回调 中使用）
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (!encoder_busy) {
        example_isp_buffer_swap(ctx);
        current_frame = example_isp_buffer_get_dsi_buffer(ctx);
        encoder_busy = true;
        xSemaphoreGiveFromISR(frame_sync_sem, &xHigherPriorityTaskWoken);
    } else {
        ESP_LOGD(TAG, "Frame dropped because encoder is busy");
    }

    // 如果有高优先级任务被唤醒，进行上下文切换
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
    return false;
}

esp_h264_enc_handle_t enc = NULL;
esp_h264_enc_in_frame_t src_frame = {0};
esp_h264_enc_out_frame_t enc_frame = {0};

esp_h264_err_t h264_encoder_init() {
    esp_h264_enc_cfg_hw_t enc_cfg = {
        .gop = 6,
        .fps = 15,
        .res = {.width = CONFIG_CAM_H, .height = CONFIG_CAM_V},
        .rc = {
            .bitrate = 1500000,
            .qp_min = 16,
            .qp_max = 36,
        },
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
    };
    esp_h264_err_t ret = ESP_H264_ERR_OK;
    size_t frame_size = CONFIG_CAM_H * CONFIG_CAM_V * 3 / 2;
    enc_frame.raw_data.buffer = esp_h264_aligned_calloc(16,
                                                        1,
                                                        frame_size,
                                                        &enc_frame.raw_data.len,
                                                        ESP_H264_MEM_SPIRAM);
    if (!enc_frame.raw_data.buffer) {
        goto cleanup;
    }

    ret = esp_h264_enc_hw_new(&enc_cfg, &enc);
    if (ret != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "Failed to create H264 encoder (error: %d)", ret);
        goto cleanup;
    }
    ret = esp_h264_enc_open(enc);
    if (ret != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open H264 encoder (error: %d)", ret);
        goto cleanup;
    }

    return ESP_H264_ERR_OK;

cleanup:
    // Cleanup encoder
    esp_h264_enc_close(enc);
    esp_h264_enc_del(enc);
    // Free memory buffers
    if (enc_frame.raw_data.buffer) {
        esp_h264_free(enc_frame.raw_data.buffer);
    }
    ESP_LOGI(TAG, "H264 process %s", (ret == ESP_H264_ERR_OK) ? "Completed successfully" : "Failed");
    return ESP_H264_ERR_FAIL;
}

example_pingpong_buffer_ctx_t pp_ctx = {0};
void camera_init() {
    // 创建同步信号量
    frame_sync_sem = xSemaphoreCreateBinary();
    if (frame_sync_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create frame sync sem");
        return;
    }

    esp_err_t ret = ESP_FAIL;
    void* fb0 = NULL;
    void* fb1 = NULL;
    size_t frame_buffer_size = 0;

    //---------------MIPI LDO Init------------------//
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = 3,
        .voltage_mv = 2500,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));

    //---------------Init with Dual Frame Buffers------------------//
    frame_buffer_size = CONFIG_CAM_H * CONFIG_CAM_V * 3 / 2;
    fb0 = heap_caps_aligned_alloc(64, frame_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    fb1 = heap_caps_aligned_alloc(64, frame_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);

    ESP_LOGI(TAG, "Original CSI resolution: %dx%d", CONFIG_CAM_H, CONFIG_CAM_V);
    ESP_LOGI(TAG, "Frame buffers: fb0=%p, fb1=%p", fb0, fb1);

    //---------------Ping-Pong Buffer Init------------------//
    ret = example_isp_buffer_init(&pp_ctx, fb0, fb1, CONFIG_CAM_H, CONFIG_CAM_V);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize buffer: %d", ret);
        return;
    }

    //---------------Camera Sensor and SCCB Init------------------//
    example_sensor_handle_t sensor_handle = {
        .sccb_handle = NULL,
        .i2c_bus_handle = NULL,
    };
    example_sensor_config_t cam_sensor_config = {
        .i2c_port_num = I2C_NUM_0,
        .i2c_sda_io_num = EXAMPLE_MIPI_CSI_CAM_SCCB_SDA_IO,
        .i2c_scl_io_num = EXAMPLE_MIPI_CSI_CAM_SCCB_SCL_IO,
        .port = ESP_CAM_SENSOR_MIPI_CSI,
        .format_name = "MIPI_2lane_24Minput_RAW10_1920x1080_30fps",
    };
    example_sensor_init(&cam_sensor_config, &sensor_handle);

    //---------------CSI Init------------------//
    esp_cam_ctlr_csi_config_t csi_config = {
        .ctlr_id = 0,
        .h_res = CONFIG_CAM_H,
        .v_res = CONFIG_CAM_V,
        .lane_bit_rate_mbps = EXAMPLE_MIPI_CSI_LANE_BITRATE_MBPS,
        .input_data_color_type = CAM_CTLR_COLOR_RAW10,
        .output_data_color_type = CAM_CTLR_COLOR_RAW10,
        .data_lane_num = 2,
        .byte_swap_en = false,
        .queue_items = 1,
    };
    esp_cam_ctlr_handle_t handle = NULL;
    ret = esp_cam_new_csi_ctlr(&csi_config, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CSI init fail[%d]", ret);
        return;
    }

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = s_camera_get_new_vb,
        .on_trans_finished = s_camera_get_finished_trans,
    };
    if (esp_cam_ctlr_register_event_callbacks(handle, &cbs, &pp_ctx) != ESP_OK) {
        ESP_LOGE(TAG, "Camera callbacks register fail");
        return;
    }

    ESP_ERROR_CHECK(esp_cam_ctlr_enable(handle));

    //---------------ISP Processor Init------------------//
    isp_proc_handle_t isp_proc = NULL;
    esp_isp_processor_cfg_t isp_config = {
        .clk_hz = 80 * 1000 * 1000,
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type = ISP_COLOR_RAW10,
        .output_data_color_type = ISP_COLOR_YUV420,
        .has_line_start_packet = false,
        .has_line_end_packet = false,
        .h_res = CONFIG_CAM_H,
        .v_res = CONFIG_CAM_V,
    };
    ret = example_create_isp_processor(&isp_config, &isp_proc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ISP pipeline init fail[%d]", ret);
        return;
    }

    //---------------ISP Pipeline Configuration------------------//
    // Initialize all ISP processing modules
    ret = example_isp_init_all_pipelines(isp_proc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ISP pipeline modules init fail[%d]", ret);
        return;
    }

    // Initialize both frame buffers to white
    memset(fb0, 0xFF, frame_buffer_size);
    memset(fb1, 0xFF, frame_buffer_size);
    esp_cache_msync((void*) fb0, frame_buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_cache_msync((void*) fb1, frame_buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    //---------------Start Camera------------------//
    if (esp_cam_ctlr_start(handle) != ESP_OK) {
        ESP_LOGE(TAG, "Camera start fail");
        return;
    }
    //-------------h264 encoder-----------------//
    h264_encoder_init();
}

uint32_t last_frames_received = 0;

void get_h264_nalu(char** buf, size_t* len, uint32_t* out_dts, uint32_t* out_pts) {
    *buf = NULL;
    *len = 0;

    if (xSemaphoreTake(frame_sync_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    if (current_frame != NULL) {
        src_frame.raw_data.buffer = (uint8_t*) current_frame;
        src_frame.raw_data.len = CONFIG_CAM_H * CONFIG_CAM_V * 3 / 2;

        esp_h264_err_t ret = esp_h264_enc_process(enc, &src_frame, &enc_frame);

        if (ret == ESP_H264_ERR_OK) {
            *buf = (char*) enc_frame.raw_data.buffer;
            *len = enc_frame.length;

            uint32_t relative_timestamp = get_stream_timestamp();

            *out_dts = relative_timestamp;
            *out_pts = relative_timestamp;

            last_frames_received = frames_received;
        } else {
            ESP_LOGE(TAG, "H264 encoding failed (%d)", ret);
        }
    }

    encoder_busy = false;
}