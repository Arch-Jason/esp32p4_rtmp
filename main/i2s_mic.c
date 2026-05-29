#include "i2s_mic.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>

#include "aacenc_lib.h"
#include "es8311_reg.h"
#include "esp_log.h"
#include "rtmp_tcp.h"
#include <freertos/semphr.h>

#define EXAMPLE_SAMPLE_RATE 11025 / 2
#define EXAMPLE_CHANNELS 1
#define AAC_FRAME_SAMPLES 1024 // AAC-LC 单物理通道一帧固定 1024 采样

// 双声道一帧整的字节数：1024 * 2 通道 * 2 字节 = 4096 字节
#define EXAMPLE_RECV_BUF_SIZE (AAC_FRAME_SAMPLES * EXAMPLE_CHANNELS * sizeof(int16_t))

static const char* TAG = "i2s_es8311";
static i2s_chan_handle_t rx_handle = NULL;
static i2s_chan_handle_t tx_handle = NULL;
static const char err_reason[][30] = {"input param is invalid", "operation timeout"};
AACENC_InfoStruct info = {0};
extern i2c_master_bus_handle_t i2c_bus_handle;
static i2c_master_dev_handle_t es8311_dev_handle = NULL;

static void gpio_init(void) {
    // 配置GPIO48为输出模式
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_OUTPUT_PA), // 选择GPIO48
        .mode = GPIO_MODE_OUTPUT,                 // 配置为输出模式
        .pull_down_en = GPIO_PULLDOWN_DISABLE,    // 禁用下拉
        .pull_up_en = GPIO_PULLUP_DISABLE,        // 禁用上拉
        .intr_type = GPIO_INTR_DISABLE            // 禁用中断
    };
    gpio_config(&io_conf);

    // 设置GPIO48为高电平
    gpio_set_level(GPIO_OUTPUT_PA, 1);
}

/* 时钟分频系数表 */

#define ES8311_I2C_ADDR 0x18 // 根据你硬件上的 ADDR 引脚确定（0x18 或 0x1B）
#define I2C_TIMEOUT_MS 1000

/* 使用总线句柄重写的写寄存器函数 */
/* 使用 i2c_master_transmit 写寄存器 */
static inline esp_err_t local_es8311_write_reg(uint8_t reg_addr, uint8_t data) {
    const uint8_t write_buf[2] = {reg_addr, data};
    // 最后一个参数是超时时间（单位：毫秒）
    return i2c_master_transmit(es8311_dev_handle, write_buf, sizeof(write_buf), 1000);
}

/* 使用 i2c_master_transmit_receive 读寄存器 */
static inline esp_err_t local_es8311_read_reg(uint8_t reg_addr, uint8_t* reg_value) {
    // 先发送寄存器地址，随后接收 1 字节数据
    return i2c_master_transmit_receive(es8311_dev_handle, &reg_addr, 1, reg_value, 1, 1000);
}

/* 匹配时钟系数的内部辅助函数 */
static int local_get_coeff(uint32_t mclk, uint32_t rate) {
    for (int i = 0; i < (sizeof(coeff_div) / sizeof(coeff_div[0])); i++) {
        if (coeff_div[i].rate == rate && coeff_div[i].mclk == mclk) {
            return i;
        }
    }
    return -1;
}

/* 内部直接配置采样率分频的函数 */
static esp_err_t local_es8311_sample_frequency_config(int mclk_frequency, int sample_frequency) {
    uint8_t regv;
    int coeff = local_get_coeff(mclk_frequency, sample_frequency);

    if (coeff < 0) {
        ESP_LOGE(TAG,
                 "Unable to configure sample rate %dHz with %dHz MCLK",
                 sample_frequency,
                 mclk_frequency);
        return ESP_ERR_INVALID_ARG;
    }

    const struct _coeff_div* const selected_coeff = &coeff_div[coeff];

    /* register 0x02 */
    ESP_RETURN_ON_ERROR(local_es8311_read_reg(ES8311_CLK_MANAGER_REG02, &regv),
                        TAG,
                        "I2C read/write error");
    regv &= 0x07;
    regv |= (selected_coeff->pre_div - 1) << 5;
    regv |= selected_coeff->pre_multi << 3;
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_CLK_MANAGER_REG02, regv),
                        TAG,
                        "I2C read/write error");

    /* register 0x03 */
    const uint8_t reg03 = (selected_coeff->fs_mode << 6) | selected_coeff->adc_osr;
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_CLK_MANAGER_REG03, reg03),
                        TAG,
                        "I2C read/write error");

    /* register 0x04 */
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_CLK_MANAGER_REG04, selected_coeff->dac_osr),
                        TAG,
                        "I2C read/write error");

    /* register 0x05 */
    const uint8_t reg05 = ((selected_coeff->adc_div - 1) << 4) | (selected_coeff->dac_div - 1);
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_CLK_MANAGER_REG05, reg05),
                        TAG,
                        "I2C read/write error");

    /* register 0x06 */
    ESP_RETURN_ON_ERROR(local_es8311_read_reg(ES8311_CLK_MANAGER_REG06, &regv),
                        TAG,
                        "I2C read/write error");
    regv &= 0xE0;
    if (selected_coeff->bclk_div < 19) {
        regv |= (selected_coeff->bclk_div - 1) << 0;
    } else {
        regv |= (selected_coeff->bclk_div) << 0;
    }
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_CLK_MANAGER_REG06, regv),
                        TAG,
                        "I2C read/write error");

    /* register 0x07 */
    ESP_RETURN_ON_ERROR(local_es8311_read_reg(ES8311_CLK_MANAGER_REG07, &regv),
                        TAG,
                        "I2C read/write error");
    regv &= 0xC0;
    regv |= selected_coeff->lrck_h << 0;
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_CLK_MANAGER_REG07, regv),
                        TAG,
                        "I2C read/write error");

    /* register 0x08 */
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_CLK_MANAGER_REG08, selected_coeff->lrck_l),
                        TAG,
                        "I2C read/write error");

    return ESP_OK;
}

#define ES8311_ADDRESS_0 0x18u

/* 重写后的不依赖库初始化函数 */
static esp_err_t es8311_codec_init(void) {
    // /* 1. 初始化新版 I2C 总线（Master Bus） */
    // i2c_master_bus_config_t i2c_bus_config = {
    //     .clk_source = I2C_CLK_SRC_DEFAULT,
    //     .i2c_port = I2C_NUM,
    //     .scl_io_num = I2C_SCL_IO,
    //     .sda_io_num = I2C_SDA_IO,
    //     .flags.enable_internal_pullup = true, // 代替原有的 GPIO_PULLUP_ENABLE
    // };
    // ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_bus_config, &i2c_bus_handle), TAG, "Initialize I2C bus failed");

    /* 2. 将 ES8311 作为一个从机设备挂载到总线上 */
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_ADDRESS_0,
        .scl_speed_hz = 100000, // 100kHz
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus_handle, &dev_config, &es8311_dev_handle),
                        TAG,
                        "Add I2C device failed");

    /* 2. 采样率范围校验 */
    ESP_RETURN_ON_FALSE((16000 >= 8000) && (16000 <= 96000),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "ES8311 init needs frequency in interval [8000; 96000] Hz");

    /* 3. 复位 ES8311 芯片 (原 es8311_init 中的复位部分) */
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_RESET_REG00, 0x1F),
                        TAG,
                        "I2C read/write error");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_RESET_REG00, 0x00),
                        TAG,
                        "I2C read/write error");
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_RESET_REG00, 0x80),
                        TAG,
                        "I2C read/write error"); // 核心上电命令

    /* 4. 时钟配置 (原 es8311_clock_config 展开) */
    // mclk_from_mclk_pin = true 且 mclk_inverted = false 对应的默认寄存器值
    uint8_t reg01 = 0x3F; // 使能所有内部时钟
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_CLK_MANAGER_REG01, reg01),
                        TAG,
                        "I2C read/write error");

    uint8_t reg06;
    ESP_RETURN_ON_ERROR(local_es8311_read_reg(ES8311_CLK_MANAGER_REG06, &reg06),
                        TAG,
                        "I2C read/write error");
    reg06 &= ~BIT(5); // sclk_inverted = false
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_CLK_MANAGER_REG06, reg06),
                        TAG,
                        "I2C read/write error");

    /* 5. 串行音频格式配置 (原 es8311_fmt_config 展开，固定为 16-bit 模式) */
    uint8_t reg00;
    ESP_RETURN_ON_ERROR(local_es8311_read_reg(ES8311_RESET_REG00, &reg00),
                        TAG,
                        "I2C read/write error");
    reg00 &= 0xBF; // 配置为从机串行接口模式
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_RESET_REG00, reg00),
                        TAG,
                        "I2C read/write error");

    uint8_t reg09 = 0; // SDP In
    uint8_t reg0a = 0; // SDP Out
    // 原对应于 16-bit 分辨率的位移：*reg |= (3 << 2);
    reg09 |= (3 << 2);
    reg0a |= (3 << 2);
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_SDPIN_REG09, reg09),
                        TAG,
                        "I2C read/write error");
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_SDPOUT_REG0A, reg0a),
                        TAG,
                        "I2C read/write error");

    /* 6. 通用模拟及电源参数初始化 (原 es8311_init 的基础寄存器序列) */
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_SYSTEM_REG0D, 0x01),
                        TAG,
                        "I2C read/write error"); // 开启模拟电路电源
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_SYSTEM_REG0E, 0x02),
                        TAG,
                        "I2C read/write error"); // 开启模拟 PGA 和 ADC 调制器
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_SYSTEM_REG12, 0x00),
                        TAG,
                        "I2C read/write error"); // 开启 DAC 电源
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_SYSTEM_REG13, 0x10),
                        TAG,
                        "I2C read/write error"); // 使能耳机驱动输出
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_ADC_REG1C, 0x6A),
                        TAG,
                        "I2C read/write error"); // 绕过 ADC 均衡器，消除数字域 DC 偏移
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_DAC_REG37, 0x08),
                        TAG,
                        "I2C read/write error"); // 绕过 DAC 均衡器

    /* 7. 根据乘数重新计算并设置采样频率 (对应原本的 es8311_sample_frequency_config) */
    ESP_RETURN_ON_ERROR(local_es8311_sample_frequency_config(16000 * EXAMPLE_MCLK_MULTIPLE, 16000),
                        TAG,
                        "set es8311 sample frequency failed");

    /* 8. 音量配置 (原 es8311_voice_volume_set 展开) */
    int volume = 50;
    if (volume < 0)
        volume = 0;
    if (volume > 100)
        volume = 100;
    int reg32 = (volume == 0) ? 0 : (((volume) * 256 / 100) - 1);
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_DAC_REG32, reg32),
                        TAG,
                        "set es8311 volume failed");

    /* 9. 麦克风输入配置 (原 es8311_microphone_config 展开，此处传参为 false 模拟麦克风) */
    uint8_t reg14 = 0x1A; // 使能模拟 MIC 并设置最大 PGA 增益
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_ADC_REG17, 0xC8),
                        TAG,
                        "set es8311 microphone failed"); // 设置基本增益
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_SYSTEM_REG14, reg14),
                        TAG,
                        "set es8311 microphone failed");

    /* 10. 麦克风增益精细化调节 (原 es8311_microphone_gain_set 展开) */
    ESP_RETURN_ON_ERROR(local_es8311_write_reg(ES8311_ADC_REG16, EXAMPLE_MIC_GAIN),
                        TAG,
                        "set es8311 microphone gain failed");

    return ESP_OK;
}

static esp_err_t i2s_driver_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; // Auto clear the legacy data in the DMA buffer
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));
    // ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(EXAMPLE_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_MCK_IO,
            .bclk = I2S_BCK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din = I2S_DI_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = EXAMPLE_MCLK_MULTIPLE;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    return ESP_OK;
}

static void i2s_mic(void* args) {
    HANDLE_AACENCODER aac_enc = (HANDLE_AACENCODER) args;

    int16_t* mic_data = malloc(EXAMPLE_RECV_BUF_SIZE);
    if (!mic_data) {
        ESP_LOGE(TAG, "No memory for PCM buffer");
        vTaskDelete(NULL);
        return;
    }

    uint8_t* aac_buf = malloc(4096);
    if (!aac_buf) {
        ESP_LOGE(TAG, "No memory for AAC buffer");
        free(mic_data);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "AAC encoding start");

    while (1) {
        size_t bytes_read = 0;

        esp_err_t ret = i2s_channel_read(rx_handle,
                                         mic_data,
                                         EXAMPLE_RECV_BUF_SIZE,
                                         &bytes_read,
                                         1000);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s read failed");
            continue;
        }

        // /* Write sample data to earphone */
        // size_t bytes_write;
        // ret = i2s_channel_write(tx_handle, mic_data, EXAMPLE_RECV_BUF_SIZE, &bytes_write, 1000);
        // if (ret != ESP_OK) {
        //     ESP_LOGE(TAG, "[echo] i2s write failed, %s", err_reason[ret == ESP_ERR_TIMEOUT]);
        //     abort();
        // }
        // if (bytes_read != bytes_write) {
        //     ESP_LOGW(TAG, "[echo] %d bytes read but only %d bytes are written", bytes_read, bytes_write);
        // }

        //----------------------------------------
        // AAC encode
        //----------------------------------------

        AACENC_BufDesc in_buf = {0};
        AACENC_BufDesc out_buf = {0};

        AACENC_InArgs in_args = {0};
        AACENC_OutArgs out_args = {0};

        void* in_ptr = mic_data;
        void* out_ptr = aac_buf;

        int in_identifier = IN_AUDIO_DATA;
        int in_size = bytes_read;
        int in_elem_size = sizeof(int16_t);

        int out_identifier = OUT_BITSTREAM_DATA;
        int out_size = 4096;
        int out_elem_size = sizeof(uint8_t);

        // mono:
        // 2048 bytes / 2 bytes per sample
        // = 1024 samples

        in_args.numInSamples = bytes_read / sizeof(uint16_t);

        in_buf.numBufs = 1;
        in_buf.bufs = &in_ptr;
        in_buf.bufferIdentifiers = &in_identifier;
        in_buf.bufSizes = &in_size;
        in_buf.bufElSizes = &in_elem_size;

        out_buf.numBufs = 1;
        out_buf.bufs = &out_ptr;
        out_buf.bufferIdentifiers = &out_identifier;
        out_buf.bufSizes = &out_size;
        out_buf.bufElSizes = &out_elem_size;

        AACENC_ERROR err = aacEncEncode(aac_enc, &in_buf, &out_buf, &in_args, &out_args);

        if (err != AACENC_OK) {
            ESP_LOGE(TAG, "aacEncEncode failed: %d", err);
            continue;
        }

        // for (int i = 0; i < out_args.numOutBytes; i++) printf("%02x", aac_buf[i]);

        //----------------------------------------
        // FLV mux
        //----------------------------------------

        if (out_args.numOutBytes > 0) {
            uint32_t a_pts = get_stream_timestamp();
            if (xSemaphoreTake(rtmp_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                int r = flv_muxer_aac(flv_muxer, aac_buf, out_args.numOutBytes, a_pts, a_pts);
                xSemaphoreGive(rtmp_mutex);

                if (r != 0) {
                    ESP_LOGE(TAG, "flv_muxer_aac failed: %d", r);
                }
            } else {
                // ESP_LOGW(TAG, "Missed audio frame due to lock timeout");
            }
        }

        // vTaskDelay(1);
    }

    free(mic_data);
    free(aac_buf);

    vTaskDelete(NULL);
}

bool i2s_mic_init(void) {
    gpio_init();
    /* Initialize i2s peripheral */
    if (i2s_driver_init() != ESP_OK) {
        ESP_LOGE(TAG, "i2s driver init failed");
        // abort();
    } else {
        ESP_LOGI(TAG, "i2s driver init success");
    }

    /* Initialize i2c peripheral and config es8311 codec by i2c */
    if (es8311_codec_init() != ESP_OK) {
        ESP_LOGE(TAG, "es8311 codec init failed");
        // abort();
    } else {
        ESP_LOGI(TAG, "es8311 codec init success");
    }

    // AAC Encoder Init
    HANDLE_AACENCODER aac_enc = NULL;
    if (aacEncOpen(&aac_enc, 0, 0) != AACENC_OK)
        return false;

    if (aacEncoder_SetParam(aac_enc, AACENC_AOT, 2) != AACENC_OK
        || aacEncoder_SetParam(aac_enc, AACENC_SAMPLERATE, 11025) != AACENC_OK
        || aacEncoder_SetParam(aac_enc, AACENC_CHANNELMODE, 1) != AACENC_OK
        || aacEncoder_SetParam(aac_enc, AACENC_BITRATE, 24000) != AACENC_OK
        || aacEncoder_SetParam(aac_enc, AACENC_TRANSMUX, TT_MP4_ADTS) != AACENC_OK
        || aacEncoder_SetParam(aac_enc, AACENC_AFTERBURNER, 0) != AACENC_OK)
        return false;

    if (aacEncEncode(aac_enc, NULL, NULL, NULL, NULL) != AACENC_OK)
        return false;

    aacEncInfo(aac_enc, &info);

    xTaskCreate(i2s_mic, "i2s_mic", 8192, aac_enc, 5, NULL);
    return true;
}
