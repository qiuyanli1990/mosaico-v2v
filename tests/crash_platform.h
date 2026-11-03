#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SOC_CPU_CORES_NUM 2
#define RTC_NOINIT_ATTR
#define IRAM_ATTR
typedef enum { ESP_RST_POWERON, ESP_RST_BROWNOUT, ESP_RST_SW, ESP_RST_WDT } esp_reset_reason_t;
typedef enum {
    PANIC_EXCEPTION_DEBUG, PANIC_EXCEPTION_IWDT, PANIC_EXCEPTION_TWDT,
    PANIC_EXCEPTION_ABORT, PANIC_EXCEPTION_FAULT,
} panic_exception_t;
typedef struct { uint32_t mepc, ra, sp, mcause, mtval; } RvExcFrame;
typedef struct { int core; panic_exception_t exception; const void *frame; } panic_info_t;
typedef struct { unsigned char app_elf_sha256[32]; } esp_app_desc_t;
extern bool g_panic_abort;
extern void *g_exc_frames[SOC_CPU_CORES_NUM];
const esp_app_desc_t *esp_app_get_description(void);
esp_reset_reason_t esp_reset_reason(void);
unsigned esp_rom_get_reset_reason(unsigned core);
int64_t esp_timer_get_time(void);
