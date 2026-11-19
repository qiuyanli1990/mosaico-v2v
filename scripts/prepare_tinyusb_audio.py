#!/usr/bin/env python3
"""Prepare the pinned TinyUSB audio driver without changing managed sources."""
import argparse
from pathlib import Path


def prepare(source: str) -> str:
    changes = [
        ('  // Send everything in ISO EP FIFO\n  uint16_t n_bytes_tx;',
         '  // Send everything in ISO EP FIFO\n  uint16_t n_bytes_tx;\n'
         '  usb_uac_trace_mic_fifo(tu_fifo_count(&audio->ep_in_ff),\n'
         '      audio->packet_sz_tx[0]);'),
        ('#include "audio_device.h"',
         '#include "audio_device.h"\n#include "usb_uac_trace.h"'),
        ('  tu_fifo_read_n(&audio->ep_in_ff, audio->lin_buf_in, n_bytes_tx);',
         '  // Short reads must never transmit stale bytes from the previous packet.\n'
         '  n_bytes_tx = tu_fifo_read_n(&audio->ep_in_ff, audio->lin_buf_in, n_bytes_tx);'),
        ('  TU_VERIFY(tu_fifo_write_n(&audio->ep_out_ff, audio->lin_buf_out, n_bytes_received));',
         '  // An empty isochronous packet must still rearm OUT reception.\n'
         '  if (n_bytes_received != 0) {\n'
         '    TU_VERIFY(tu_fifo_write_n(&audio->ep_out_ff, audio->lin_buf_out, n_bytes_received));\n'
         '  }'),
        ('            // Schedule first feedback transmit\n            audiod_fb_send(audio);',
         '            // First feedback is submitted after its value and format are configured.'),
        ('      // Prepare feedback computation if endpoint is available\n      if (audio->ep_fb != 0) {',
         '      // Configure feedback only when opening its speaker interface.\n'
         '      if (audio->ep_fb != 0 && audio->ep_out_as_intf_num == itf && alt != 0) {'),
        ('        }\n      }\n#endif// CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP\n\n      // We are done',
         '        }\n        TU_VERIFY(audiod_fb_send(audio));\n      }\n'
         '#endif// CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP\n\n      // We are done'),
        ('      TU_VERIFY(tud_audio_set_itf_cb(rhport, p_request));',
         '      TU_VERIFY(tud_audio_set_itf_cb(rhport, p_request));\n'
         '      usb_uac_trace_interface(itf, alt);'),
        ('bool audiod_xfer_isr(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {',
         'bool audiod_xfer_isr(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {\n'
         '  usb_uac_trace_done(ep_addr, result, xferred_bytes);'),
    ]
    for original, replacement in changes:
        if source.count(original) != 1:
            raise ValueError("Pinned TinyUSB audio driver changed; review the integration patch: " + original)
        source = source.replace(original, replacement)
    return source.replace("usbd_edpt_xfer(", "usb_uac_trace_xfer(")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    content = prepare(args.source.read_text())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text() != content:
        args.output.write_text(content)


if __name__ == "__main__":
    main()
