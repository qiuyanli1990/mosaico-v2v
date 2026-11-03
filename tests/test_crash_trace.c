#include <assert.h>
#include <setjmp.h>
#include "crash_trace.c"

bool g_panic_abort;
void *g_exc_frames[SOC_CPU_CORES_NUM];
static esp_reset_reason_t reset = ESP_RST_POWERON;
static int64_t now_us;
static jmp_buf panic_exit;
static const esp_app_desc_t app = {.app_elf_sha256 = {1, 2, 3, 4}};
const esp_app_desc_t *esp_app_get_description(void) { return &app; }
esp_reset_reason_t esp_reset_reason(void) { return reset; }
unsigned esp_rom_get_reset_reason(unsigned core) { return 16 + core; }
int64_t esp_timer_get_time(void) { return now_us; }
void __real_esp_panic_handler(panic_info_t *info) { (void)info; longjmp(panic_exit, 1); }

int main(void)
{
    char line[512];
    crash_trace_init(0x1234);
    crash_trace_format_previous(line, sizeof(line));
    assert(strstr(line, "previous=none"));
    now_us = 35019000;
    crash_trace_mark(CRASH_TRACE_SESSION, CRASH_STAGE_JOIN, 1);
    crash_trace_mark(CRASH_TRACE_SIGNAL_SPEAKER, CRASH_STAGE_HTTP_BEGIN, 0);
    /* An interrupted checkpoint must not be presented as a valid stage. */
    retained.marks[CRASH_TRACE_AUDIO].check = 0;
    retained.marks[CRASH_TRACE_AUDIO].stage = CRASH_STAGE_AEC_RESET;
    RvExcFrame frame = {.mepc = 0x50012340, .ra = 0x50012338, .sp = 0x48000000,
                        .mcause = 7, .mtval = 0xdeadbeef};
    panic_info_t info = {.core = 1, .exception = PANIC_EXCEPTION_FAULT, .frame = &frame};
    if (!setjmp(panic_exit)) __wrap_esp_panic_handler(&info);
    /* Preserve the first panic even if the UART panic handler then fails. */
    frame.mepc = 0x55555555;
    if (!setjmp(panic_exit)) __wrap_esp_panic_handler(&info);
    reset = ESP_RST_WDT;
    now_us = 0;
    crash_trace_init(0x5678);
    size_t size = crash_trace_format_previous(line, sizeof(line));
    assert(size == strlen(line) && size < sizeof(line) - 1);
    assert(strstr(line, "raw=16/17 previous=yes boot=00001234"));
    assert(strstr(line, "s1=3/1@35019ms"));
    assert(strstr(line, "s2=6/0@35019ms"));
    assert(!strstr(line, "s4="));
    assert(strstr(line, "panic=4 core=1"));
    assert(strstr(line, "pc=50012340,ra=50012338,sp=48000000,cause=00000007,val=deadbeef"));
    char short_line[4];
    assert(crash_trace_format_previous(short_line, sizeof(short_line)) == 3);
    assert(short_line[3] == '\0');
    assert(crash_trace_format_previous(NULL, 0) == 0);
    /* The last boot's panic must not leak into a subsequent healthy boot. */
    crash_trace_init(0x9abc);
    crash_trace_format_previous(line, sizeof(line));
    assert(strstr(line, "boot=00005678") && strstr(line, "panic=not-recorded"));
    reset = ESP_RST_POWERON;
    crash_trace_init(0xdef0);
    crash_trace_format_previous(line, sizeof(line));
    assert(strstr(line, "previous=none"));
    puts("PASS: reset trace: first panic retained, register replay, torn checkpoint rejected, bounded output, stale/power-on records cleared");
    return 0;
}
