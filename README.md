# Mosaico USB Translator

Standalone ESP32-S31 firmware for Agora V2V translation, with a touchscreen,
USB audio and three translation modes.

| Mode | Audio source and behavior | V2V rooms |
| --- | --- | --- |
| Media | Translate playback from a USB-connected computer or phone; hear the translation on Mosaico | 1 |
| Live meeting | Translate meeting audio to you and your microphone audio back to the meeting | 2 |
| Travel | Tap a language card to speak through the onboard microphone; hear the translation locally | 2 |

This source snapshot includes the verified M1/macOS 26.7.1 Media fix:
USB revision `0x0105`, profile `hs-feedback-1ms-v3`.
See [source provenance](SOURCE.md) and [USB audio configuration](docs/usb-audio.md).

## Hardware and services

- An ESP32-S31 Mosaico board matching the pinned BSP, with its LCD, touch,
  ES8311 audio codec, 16 MB flash and octal PSRAM configuration.
- A USB data cable connected to the board's USB OTG connector for Media and
  Live meeting. Travel uses the onboard microphone.
- Wi-Fi with internet access, plus an Agora project and REST customer
  credentials with access to the configured V2V and duplex WHIP services.
- A certificate-disabled RTC project. Certificate-enabled RTC token generation
  is not implemented by this firmware.

The default endpoints are the Agora AP Southeast Asia V2V API and duplex WHIP
gateway. Service availability, permissions and supported TTS voices depend on
your account. Translation requires cloud connectivity.

## Download the build environment

The ESP-IDF checkout, compiler, Python environment, downloaded dependencies and
build outputs are **not included in this repository**. The following commands
download them into the cloned project's own directories.

The documented host is macOS. Install Python 3.10 or newer, Git, CMake, Ninja
and Xcode Command Line Tools before starting. Host tests also require a C
compiler with AddressSanitizer and UndefinedBehaviorSanitizer support.
With Homebrew, the additional tools can be installed with:

```sh
brew install python cmake ninja
```

Install Xcode Command Line Tools if they are not already installed:

```sh
xcode-select --install
```

Clone this repository, change into its directory, then run:

```sh
python3 scripts/bootstrap.py --install-idf
```

Network access to GitHub, Espressif's download servers, the ESP Component
Registry and Python package indexes is required. The first setup downloads
large SDK and toolchain packages and needs several gigabytes of free space.

The bootstrap fetches these exact source revisions:

| Dependency | Revision |
| --- | --- |
| ESP-IDF | `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe` |
| Mosaico BSP | `a7a93d290be28d18fbfd32361c90aa20b0cd1c67` |
| Agora WebRTC reference | `b57d626c9a885c75a216a5b2deccc6bc2fe2e045` |

Use the pinned ESP-IDF commit, which supports this preview ESP32-S31 target.
An arbitrary stable or latest ESP-IDF release is not a substitute. The wrapper
sets the expected IDF version to `6.2.0` and checks the checkout before building.
Registry component versions and hashes are recorded in `dependencies.lock`.
The wrapper also loads `scripts/component-constraints.txt`, which pins all
registry versions from the verified snapshot. This prevents automatic upgrades
when the component manager resolves conditional display dependencies during
the first configuration. Keep both files when cloning or exporting the source.
See [THIRD_PARTY.md](THIRD_PARTY.md) for integration patches and provenance.

| Local directory | Created by | Purpose |
| --- | --- | --- |
| `.tools/esp-idf/` | bootstrap | Pinned ESP-IDF source |
| `.tools/espressif/` | bootstrap | Compiler, ROM files and Python environment |
| `third_party/` | bootstrap | Pinned BSP and WebRTC source |
| `managed_components/` | ESP-IDF component manager | Registry components downloaded on first configuration |
| `build/` | build and tests | Firmware, configuration, logs and UI previews |

All these directories are ignored by Git. No existing global ESP-IDF
installation needs to be changed or activated.

## Configure and build

Create a local configuration from the placeholder-only example:

```sh
cp sdkconfig.local.example sdkconfig.local
```

Edit `sdkconfig.local` and enter your own values:

```text
CONFIG_TRANSLATOR_CUSTOMER_ID="YOUR_CUSTOMER_ID"
CONFIG_TRANSLATOR_CUSTOMER_SECRET="YOUR_CUSTOMER_SECRET"
CONFIG_TRANSLATOR_APP_ID="YOUR_APP_ID"
CONFIG_TRANSLATOR_APP_CERT=""
```

Leave `CONFIG_TRANSLATOR_APP_CERT` empty for the supported certificate-disabled
flow. REST customer ID/secret are still needed for V2V API authentication.
The firmware uses the reference gateway's WHIP authentication mechanism
separately from RTC tokens. The optional Wi-Fi fields can stay empty; configure
Wi-Fi on the touchscreen instead. Saved device Wi-Fi settings take priority
over the build-time fallback values.

`sdkconfig.local` accepts only `CONFIG_TRANSLATOR_*` settings. It is applied
on subsequent builds too. Keep it, generated `sdkconfig`, and firmware images
containing credentials out of Git.

Configure the target and build through the project wrapper:

```sh
bash scripts/idf.sh --preview set-target esp32s31
bash scripts/idf.sh build
```

The first configuration downloads the managed components. The resulting
application is `build/mosaico_translator.bin`, accompanied by its ELF, the
bootloader and partition table. An image is about 3.34 MiB and fits the 8 MiB
application partition. Build success does not contact V2V or start an Agent.

After creating a new clone, run bootstrap again. Do not copy another clone's
Python environment or build directory: they contain paths to their own clone.

## Run the tests

After a successful configuration/build has downloaded the managed components:

```sh
python3 scripts/test_host.py
python3 scripts/test_ui.py
```

The host suite runs with ASan/UBSan and covers PCM queues, audio workers,
session lifecycle, request contracts, voice selection, USB descriptors and the
actual prepared TinyUSB audio driver with a simulated USB controller.
The UI suite uses the actual LVGL renderer with mocked hardware services.
It renders previews to `build/ui-preview-v6/`; the `v6` name identifies the
touchscreen layout, which the USB compatibility update retains.
Host tests do not measure acoustic AEC quality or replace hardware playback tests.

## Install and connect

This is a standalone ESP-IDF application with its own partition table. It is
not an update package for a factory Mosaico Vibe Mode/ESP-Iris installation.
Choose an installation/recovery method suitable for your board before writing
the bootloader or partition table. Bootstrap and build do not flash a device.

On a board already installed with this application's partition layout, enter
download mode and select the download-mode serial port for an application-only
update:

```sh
bash scripts/idf.sh -p /dev/cu.usbmodemDOWNLOAD flash
```

The application then re-enumerates as a composite UAC2 audio and CDC console
device (`VID=0x303a`, `PID=0x4012`). Find the newly enumerated runtime port:

```sh
ls /dev/cu.*
bash scripts/idf.sh -p /dev/cu.usbmodemRUNTIME monitor --no-reset
```

Replace both example port names with your actual ports. Disconnect/reconnect
after updating so the host reads the USB revision again. Exit the monitor with
Ctrl+]. The firmware continues to run.

## Use the touchscreen

1. Open Wi-Fi from Home, scan, select a network and connect. The device saves
   successful credentials in NVS and reconnects after reboot.
2. Wait for Wi-Fi and time synchronization, then choose a translation mode.
3. Choose the source language, target language and target voice.
4. For Media, select `mosaico` as the computer/phone's playback output and play
   media. For Live meeting, select `mosaico` as the meeting application's
   speaker and microphone. macOS Audio MIDI Setup should show 48 kHz, 16-bit,
   stereo output and mono input.
5. Start translation. In Travel, tap a direction card to speak, tap again to
   pause, or tap the other card to change direction.
6. Pause or Return stops audio before asynchronous cloud/session cleanup.
   Wait for cleanup before starting a new configuration.

Home shows the time in UTC+8, battery, Wi-Fi and brightness. Translation pages
provide volume and brightness controls. Host mute suppresses USB translation
input; host UAC volume is separate from translation playback volume.

All modes use audio without subtitles. The language list has 60 entries;
35 currently support TTS on this provider path. Targets without TTS do not
produce spoken output. See [voice choices and language limits](docs/tts-voices.md).

## Diagnostics and limitations

The CDC banner includes the boot ID, reset cause, firmware ELF prefix and USB
profile. `USB_DIAG` prints every five seconds, including before translation
starts. During playback, compare successive OUT/APP byte counters and feedback
success counts (`FB packets - err`). See [USB diagnostics](docs/usb-audio.md).

Media creates one V2V Agent and one WebRTC connection. Live meeting and Travel
use two directions. Network loss stops the session; reconnect and start a fresh
one. Translation sessions use cloud resources under your account.

The M1/macOS 26.7.1 Media failure was confirmed fixed on USB revision `0x0105`.
Other host/device combinations, sleep/wake, long-duration stability, acoustic
AEC quality and end-to-end translation latency need hardware validation for
your setup.
