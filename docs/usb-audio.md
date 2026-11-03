# USB audio configuration and diagnostics

The USB OTG connector exposes UAC2 audio and a CDC serial console. The USB
product and audio interface names are `mosaico`. Device revision `0x0105`,
profile `hs-feedback-1ms-v3`, includes the Media compatibility repair confirmed
on an M1 Mac running macOS 26.7.1.

## Endpoints

| Endpoint | High Speed (480 Mb/s) | Full Speed (12 Mb/s) |
| --- | --- | --- |
| Speaker OUT (`0x01`) | `bInterval=1`, 125 us, maximum 28 bytes | `bInterval=1`, 1 ms, maximum 196 bytes |
| Microphone IN (`0x82`) | `bInterval=4`, 1 ms, maximum 98 bytes | `bInterval=1`, 1 ms, maximum 98 bytes |
| Feedback IN (`0x81`) | `bInterval=4`, 1 ms, four-byte 16.16 | `bInterval=1`, 1 ms, three-byte 10.14 for macOS |

At 48 kHz, 16-bit stereo, HS Speaker OUT normally carries six sample frames
per microframe: 24 bytes. The 28-byte maximum allows one extra sample frame.
The microphone is mono, so its maximum packet is 98 bytes. Speaker channels
have explicit front-left/front-right positions, and the audio function counts
both streaming interfaces.

HS feedback is fixed at `0x00060000`: six samples per 125 us microframe in
16.16 format. Changing the feedback polling interval to 1 ms does not change
the units of this value. FS uses FIFO feedback with macOS format conversion.
The HS feedback interval matches TinyUSB's
[speaker reference](https://github.com/hathach/tinyusb/blob/master/examples/device/uac2_speaker_fb/src/usb_descriptors.h).

The UAC software FIFOs retain millisecond-sized buffers. The component setting
`CONFIG_UAC_SPK_INTERVAL_MS=10` controls buffering, not USB `bInterval`.

## Driver integration

`scripts/prepare_tinyusb_audio.py` prepares a checked copy of the pinned audio
driver under `build/`. It initializes feedback value/format before the first
submission, avoids restarting speaker feedback when the independent microphone
opens, and rearms OUT after empty packets. Managed component files are not
edited. An unexpected upstream driver causes the exact-match patch to fail
during configuration instead of silently applying to different code.

## Reading runtime counters

Disconnect/reconnect after flashing and check the CDC banner for
`rev=0105 profile=hs-feedback-1ms-v3`. A changed boot ID means a reboot;
an unchanged boot ID with a later uptime means a CDC reconnection.

Select `mosaico` as playback output and play continuous speech or music for
at least 30 seconds. At 48 kHz stereo, OUT/APP bytes should increase by about
960000 per five seconds; the mono encoder consumes about 480000 samples per
ten seconds. These are approximate expected deltas, not reported measurements
from every host.

| Observation | Inspect |
| --- | --- |
| Speaker `alt=0` | Host playback output and stream selection |
| OUT bytes advance, APP bytes stop | UAC FIFO and application worker |
| OUT/APP advance, `nonzero` does not | Silent host PCM or stereo cancellation |
| APP advances, encoder PCM does not | Host mute, forwarding and capture selection |
| OUT stops, feedback successes stop | Host feedback polling and device controller path |
| `err` or `submit_fail` increases | Transfer completions or submission/rearm failures |

`FB packets` includes failed completions. Compare `FB packets - err` across
records to determine whether feedback succeeds. `submit_fail=0` means the
software accepted submissions, not that the host received them.
At HS, successful feedback must carry `value=00060000 len=4`.

`nonzero` counts nonzero mono samples before mute/forwarding. Raw counters are
32-bit and application totals are 64-bit; snapshots are approximate because
USB continues during logging. Interrupt hooks use lock-free atomics and do not
log or allocate memory.

## Validation scope

Revision `0x0104` already used 125 us/28-byte Speaker OUT packets, but its
125 us feedback endpoint accumulated failures and the host stopped supplying
PCM. Revision `0x0105` changes feedback IN to 1 ms while retaining OUT settings.
The user confirmed the reported Media issue fixed on the affected M1/macOS
26.7.1 host. This does not establish behavior for every macOS or USB controller.

Host regression tests cover HS/FS descriptors, other-speed descriptors, first
feedback values, stream reopen, independent microphone switching, zero-length
OUT rearming and transfer-error diagnostics. The controller in those tests is
simulated; real hardware validates polling, on-wire delivery and sustained audio.
