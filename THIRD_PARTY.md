# Dependency provenance

Dependencies are fetched by `scripts/bootstrap.py` into ignored directories.

| Dependency | Commit |
| --- | --- |
| coin0/esp-webrtc-solution, dev/agora-demo | b57d626c9a885c75a216a5b2deccc6bc2fe2e045 |
| esp-mosaico/esp-mosaico-bsp | a7a93d290be28d18fbfd32361c90aa20b0cd1c67 |
| espressif/esp-idf, Mosaico pinned SDK | 7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe |

`main/agora_auth.c`, `main/agora_auth.h`, `main/agora_signaling.c`, and
`main/agora_signaling.h` derive from the specified Agora demo. The authorization
adapter preserves the gateway's `stringuid` parameter and uses String UID
`12345`. V2V receives the same identifier in `remote_rtc_uids` with
`properties.enable_string_uid=true`.
See the upstream repository's license. WebRTC, rendering, BSP and managed
components retain their respective upstream licenses.

One integration patch changes av_render's esp_codec_dev requirement from
`^2.0.0-beta1` to `~1.6`, matching the BSP and capture package. The used I2S render
API is available in that series. The patch is explicit and reproducible in the
bootstrap script; no unrelated workspace dependency is modified.

`main/CMakeLists.txt` also namespaces the UAC component's four lifecycle callbacks
with private compiler definitions. Application callbacks call these original
handlers before updating session/USB state, retaining UAC's stream reset logic
without editing the managed component.

The project also selects `main/mosaico_tusb_config.h` for TinyUSB and UAC. It
includes the upstream configuration and overrides the audio streaming-interface
count to two via `main/usb_uac_config.h`. Endpoint descriptors in
`main/usb_descriptors.c` are adjusted by actual USB speed: HS speaker OUT uses
28-byte packets every 125 us, while the mono microphone keeps a 1 ms interval
and 98-byte maximum packet. FS speaker OUT remains 196 bytes every 1 ms. Speaker
channel positions are FL/FR. Component software-buffer sizes are unchanged.

The UAC component's feedback-parameter callback is privately namespaced so the
project callback can select fixed HS feedback (`0x00060000` at 48 kHz) and seed
it on stream open. FS keeps the FIFO feedback method and macOS 10.14 conversion.
The feedback endpoint advertises four bytes at HS and three at FS, with a 1 ms
polling interval at either speed (`bInterval=4` at HS, `1` at FS). The HS interval
matches TinyUSB's `examples/device/uac2_speaker_fb/src/usb_descriptors.h` reference
configuration. The user confirmed that revision `0x0105` fixes the reported
Media failure on M1/macOS 26.7.1. These changes are tracked in the application;
no managed dependency files are edited.

`scripts/prepare_tinyusb_audio.py` prepares a build-directory copy of the pinned
TinyUSB `audio_device.c`, selected by `main/CMakeLists.txt`. Exact-match checks
fail configuration if the upstream implementation changes. The overlay delays
the first feedback submission until its value and FS/HS format are configured,
limits that initialization to the speaker interface, and keeps OUT reception
armed after zero-length packets (`tu_fifo_write_n(..., 0)` returns zero and must
not abort the receive path). It also installs the lock-free counter hooks from
`main/usb_uac_trace.c` for completions, feedback values and failed submissions.
No logging or allocation runs in the interrupt hooks. Tests exercise the prepared
driver against the production descriptors and a simulated USB controller.

