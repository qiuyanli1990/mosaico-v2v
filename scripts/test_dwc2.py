from pathlib import Path
import subprocess
import tempfile
import runpy

root = Path(__file__).resolve().parents[1]
tinyusb = root / 'managed_components/espressif__tinyusb/src'
original = (tinyusb / 'portable/synopsys/dwc2/dcd_dwc2.c').read_text()
prepare = runpy.run_path(str(root / 'scripts/prepare_dwc2.py'))['prepare']
source = prepare(original)
def function(signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]
start = source.index('typedef struct {\n  uint8_t* buffer;')
xfer_type = source[start:source.index('} xfer_ctl_t;', start) + len('} xfer_ctl_t;')]
with tempfile.TemporaryDirectory(prefix='mosaico-dwc2-analysis-') as tmp:
    tmp = Path(tmp)
    config = (root / 'sdkconfig').read_text().splitlines()
    (tmp / 'sdkconfig.h').write_text('\n'.join(
        line for line in config if line.startswith('CONFIG_UAC_') or
        line.startswith('CONFIG_USB_DEVICE_UAC_AS_PART='))
        .replace('CONFIG_', '#define CONFIG_').replace('=y', ' 1').replace('=', ' ') + '\n')
    prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "portable/synopsys/dwc2/dwc2_type.h"
static dwc2_regs_t controller;
#define DWC2_REG(port) ((void)(port), &controller)
'''
    stubs = r'''
static xfer_ctl_t transfers[4][2];
#define XFER_CTL_BASE(ep, dir) (&transfers[ep][dir])
static struct { uint16_t ep0_pending[2]; } _dcd_data;
static unsigned failures, disables;
static bool dma_device_enabled(const dwc2_regs_t *regs) { (void)regs; return false; }
static bool dcd_dcache_clean(const void *data, uint32_t bytes)
{ (void)data; (void)bytes; return true; }
static uint8_t dwc2_ep_count(const dwc2_regs_t *regs) { (void)regs; return 3; }
static void edpt_disable(uint8_t port, uint8_t ep, bool stall)
{ (void)port; (void)stall; controller.epin[ep & 15].diepctl = 0; ++disables; }
static void dcd_event_xfer_complete(uint8_t port, uint8_t ep, uint32_t bytes,
                                   xfer_result_t result, bool in_isr)
{ (void)port; assert(ep == 0x82 && bytes == 0 && result == XFER_RESULT_FAILED && in_isr); ++failures; }
'''
    main = r'''
int main(void)
{
    const unsigned sizes[] = {0, 4, 94, 96, 98};
    for (unsigned c = 0; c < sizeof(sizes) / sizeof(sizes[0]); ++c) {
        memset(&controller, 0, sizeof(controller));
        memset(transfers, 0, sizeof(transfers));
        failures = disables = 0;
        xfer_ctl_t *xfer = XFER_CTL_BASE(2, TUSB_DIR_IN);
        xfer->max_size = 98;
        xfer->total_len = sizes[c];
        xfer->interval = xfer->iso_retry = 8;
        dwc2_depctl_t ctl = {.value = 0};
        ctl.type = DEPCTL_EPTYPE_ISOCHRONOUS;
        controller.epin[2].diepctl = ctl.value;
        edpt_schedule_packets(0, 2, TUSB_DIR_IN);
        dwc2_ep_tsize_t size = {.value = controller.epin[2].tsiz};
        unsigned initial_packets = size.packet_count;
        unsigned malformed_retries = 0;
        for (unsigned retry = 0; retry < 9; ++retry) {
            dwc2_dsts_t dsts = {.value = 0};
            dsts.frame_number = retry;
            controller.dsts = dsts.value;
            ctl.value = controller.epin[2].diepctl;
            ctl.enable = 1;
            ctl.type = DEPCTL_EPTYPE_ISOCHRONOUS;
            ctl.dpid_iso_odd = retry & 1;
            controller.epin[2].diepctl = ctl.value;
            handle_incomplete_iso_in(0);
            size.value = controller.epin[2].tsiz;
            if (retry < 8 && size.packet_count == 0) ++malformed_retries;
        }
        assert(initial_packets == 1 && failures == 1 && disables == 1);
        assert(malformed_retries == 0);
        printf("PASS: patched DWC2 retry registers: payload=%u bytes, "
               "initial PKTCNT=%u, retries with PKTCNT=0: %u/8, exhaustion failures=%u\n",
               sizes[c], initial_packets, malformed_retries, failures);
    }
}
'''
    (tmp / 'diagnose.c').write_text(prefix + xfer_type + stubs +
        function('static void edpt_schedule_packets(') + '\n' +
        function('static void handle_incomplete_iso_in(') + '\n' + main)
    binary = tmp / 'diagnose'
    subprocess.run([
        'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g', '-fsanitize=address,undefined',
        '-DCFG_TUSB_CONFIG_FILE="usb_config.h"', '-I', str(tmp), '-I', str(root / 'tests'),
        '-I', str(root / 'main'), '-I', str(tinyusb),
        '-I', str(root / 'managed_components/espressif__usb_device_uac/tusb_uac'),
        str(tmp / 'diagnose.c'), '-o', str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
