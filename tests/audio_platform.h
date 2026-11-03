#pragma once
/* Host shims for worker/data-path tests. Hardware initialization is compiled
 * but not exercised; ESP-IDF compilation validates the real hardware APIs. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_NOT_SUPPORTED 2
#define ESP_ERR_INVALID_ARG 3
#define ESP_ERR_INVALID_STATE 4
#define ESP_CODEC_DEV_OK 0
#define CONFIG_TRANSLATOR_AEC_REF_DELAY_MS 0
#define EXT_RAM_BSS_ATTR
#define MALLOC_CAP_8BIT 1
#define MALLOC_CAP_SPIRAM 2
#define ESP_RETURN_ON_ERROR(expr, tag, ...) do { int r = (expr); (void)(tag); if (r) return r; } while (0)
static inline void test_log(const char *tag, const char *format, ...)
{ (void)tag; (void)format; }
#define ESP_LOGI(...) test_log(__VA_ARGS__)
#define ESP_LOGE(...) test_log(__VA_ARGS__)

typedef int BaseType_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef void *QueueHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
int64_t esp_timer_get_time(void);
void *heap_caps_aligned_alloc(size_t, size_t, unsigned);
void heap_caps_free(void *);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, unsigned);
int xSemaphoreGive(SemaphoreHandle_t);
QueueHandle_t xQueueCreate(unsigned, size_t);
int xQueueSendFromISR(QueueHandle_t, const void *, BaseType_t *);
int xQueueReceive(QueueHandle_t, void *, unsigned);
void vTaskDelay(unsigned);
static inline unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 4096; }
static inline int xTaskCreateWithCaps(void (*task)(void *), const char *name, unsigned stack,
                                      void *arg, unsigned priority, TaskHandle_t *handle, unsigned caps)
{ (void)task; (void)name; (void)stack; (void)arg; (void)priority; (void)handle; (void)caps; return pdPASS; }
static inline void vTaskDeleteWithCaps(TaskHandle_t task) { (void)task; }

typedef void *esp_codec_dev_handle_t;
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t, unsigned);
int esp_codec_dev_write(esp_codec_dev_handle_t, void *, int);
typedef struct { int sample_rate, bits_per_sample, channel; } esp_codec_dev_sample_info_t;
typedef struct { int unused; } audio_codec_data_if_t;
typedef struct { int unused; } audio_codec_ctrl_if_t;
typedef struct { int unused; } audio_codec_gpio_if_t;
typedef struct { int unused; } audio_codec_if_t;
typedef struct { int port; void *rx_handle, *tx_handle; } audio_codec_i2s_cfg_t;
typedef struct { int addr; void *bus_handle; } audio_codec_i2c_cfg_t;
typedef struct {
    const audio_codec_ctrl_if_t *ctrl_if;
    const audio_codec_gpio_if_t *gpio_if;
    int codec_mode, pa_pin;
    bool use_mclk, digital_mic, no_dac_ref;
    struct { float pa_voltage, codec_dac_voltage; } hw_gain;
} es8311_codec_cfg_t;
typedef struct {
    int dev_type;
    const audio_codec_if_t *codec_if;
    const audio_codec_data_if_t *data_if;
} esp_codec_dev_cfg_t;
#define ESP_CODEC_DEV_WORK_MODE_BOTH 3
#define ESP_CODEC_DEV_TYPE_IN_OUT 3
static inline const audio_codec_data_if_t *audio_codec_new_i2s_data(audio_codec_i2s_cfg_t *p)
{ (void)p; return (void *)1; }
static inline const audio_codec_ctrl_if_t *audio_codec_new_i2c_ctrl(audio_codec_i2c_cfg_t *p)
{ (void)p; return (void *)1; }
static inline const audio_codec_gpio_if_t *audio_codec_new_gpio(void) { return (void *)1; }
static inline const audio_codec_if_t *es8311_codec_new(es8311_codec_cfg_t *p)
{ (void)p; return (void *)1; }
static inline esp_codec_dev_handle_t esp_codec_dev_new(esp_codec_dev_cfg_t *p)
{ (void)p; return (void *)1; }
static inline int esp_codec_dev_open(esp_codec_dev_handle_t p, esp_codec_dev_sample_info_t *fmt)
{ (void)p; (void)fmt; return 0; }
static inline int esp_codec_dev_close(esp_codec_dev_handle_t p) { (void)p; return 0; }
static inline int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t p, float gain)
{ (void)p; (void)gain; return 0; }

typedef void *i2s_chan_handle_t;
typedef struct { void *dma_buf; size_t size; } i2s_event_data_t;
typedef struct {
    bool (*on_recv)(i2s_chan_handle_t, i2s_event_data_t *, void *);
    bool (*on_sent)(i2s_chan_handle_t, i2s_event_data_t *, void *);
} i2s_event_callbacks_t;
typedef struct { unsigned dma_desc_num, dma_frame_num; bool auto_clear_after_cb; } i2s_chan_config_t;
typedef struct {
    struct { int sample_rate; } clk_cfg;
    struct { int slot_mask; } slot_cfg;
    struct { int mclk, bclk, ws, dout, din; } gpio_cfg;
} i2s_std_config_t;
#define I2S_CHANNEL_DEFAULT_CONFIG(id, role) {0}
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) { .sample_rate = rate }
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) {0}
#define I2S_NUM_0 0
#define I2S_ROLE_MASTER 0
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_MODE_MONO 1
#define I2S_STD_SLOT_LEFT 1
static inline int i2s_new_channel(i2s_chan_config_t *p, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx)
{ (void)p; (void)tx; (void)rx; return 0; }
static inline int i2s_channel_init_std_mode(i2s_chan_handle_t p, i2s_std_config_t *config)
{ (void)p; (void)config; return 0; }
static inline int i2s_channel_register_event_callback(i2s_chan_handle_t p, i2s_event_callbacks_t *cb, void *ctx)
{ (void)p; (void)cb; (void)ctx; return 0; }

#define BSP_AUDIO_I2S_MCLK 54
#define BSP_AUDIO_I2S_SCLK 37
#define BSP_AUDIO_I2S_LRCLK 49
#define BSP_AUDIO_I2S_SDOUT 52
#define BSP_AUDIO_I2S_DSIN 40
#define BSP_AUDIO_CODEC_I2C_ADDR 50
#define BSP_AUDIO_PA_CTRL 45
static inline int bsp_i2c_init(void) { return 0; }
static inline void *bsp_i2c_get_handle(void) { return (void *)1; }
static inline int bsp_power_set_vcc_3v3(bool on) { (void)on; return 0; }
static inline int bsp_power_set_codec_3v3(bool on) { (void)on; return 0; }

typedef struct { int unused; } aec_handle_t;
#define AEC_MODE_FD_HIGH_PERF 6
aec_handle_t *aec_create(int, int, int, int);
void aec_destroy(aec_handle_t *);
int aec_get_chunksize(aec_handle_t *);
void aec_process(const aec_handle_t *, int16_t *, int16_t *, int16_t *);
