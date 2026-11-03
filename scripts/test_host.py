#!/usr/bin/env python3
"""Run portable audio-buffer and V2V contract tests with sanitizers."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cjson-dir", type=Path,
                        default=ROOT / "managed_components/espressif__cjson/cJSON")
    args = parser.parse_args()
    cjson = args.cjson_dir
    if not (cjson / "cJSON.c").is_file():
        raise SystemExit("Configure the IDF project first, or pass --cjson-dir with cJSON.c and cJSON.h")
    with tempfile.TemporaryDirectory(prefix="mosaico-tests-") as directory:
        binary = str(Path(directory) / "test_core")
        cjson_object = str(Path(directory) / "cjson.o")
        subprocess.run([
            "cc", "-std=c11", "-g", "-fsanitize=address,undefined",
            "-Wno-deprecated-declarations", "-I", str(cjson),
            "-c", str(cjson / "cJSON.c"), "-o", cjson_object,
        ], check=True)
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
            "-fsanitize=address,undefined", "-I", str(ROOT / "main"), "-I", str(cjson),
            str(ROOT / "tests/test_core.c"), str(ROOT / "main/pcm_ring.c"),
            str(ROOT / "main/pcm_timeline.c"),
            str(ROOT / "main/translation_protocol.c"), cjson_object,
            "-o", binary,
        ], check=True)
        subprocess.run([binary], check=True)
        headers = [
            "esp_err.h", "esp_aec.h", "esp_heap_caps.h", "esp_attr.h", "esp_log.h",
            "esp_check.h", "esp_timer.h", "esp_codec_dev_defaults.h", "bsp/esp_mosaico.h",
            "driver/i2s_std.h", "freertos/FreeRTOS.h", "freertos/queue.h",
            "freertos/semphr.h", "freertos/task.h",
        ]
        platform = ROOT / "tests/audio_platform.h"
        for name in headers:
            header = Path(directory) / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text(f'#include "{platform}"\n')
        audio_binary = str(Path(directory) / "test_board_audio")
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
            "-fsanitize=address,undefined", "-I", directory, "-I", str(ROOT / "main"),
            str(ROOT / "tests/test_board_audio.c"), str(ROOT / "main/aec_processor.c"),
            str(ROOT / "main/pcm_ring.c"), str(ROOT / "main/pcm_timeline.c"),
            "-o", audio_binary,
        ], check=True)
        subprocess.run([audio_binary], check=True)
        crash_headers = [
            "esp_app_desc.h", "esp_attr.h", "esp_private/panic_internal.h",
            "esp_rom_sys.h", "esp_system.h", "esp_timer.h", "riscv/rvruntime-frames.h",
        ]
        crash_shims = Path(directory) / "crash"
        for name in crash_headers:
            header = crash_shims / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text(f'#include "{ROOT / "tests/crash_platform.h"}"\n')
        crash_binary = str(Path(directory) / "test_crash_trace")
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
            "-fsanitize=address,undefined", "-I", str(crash_shims), "-I", str(ROOT / "main"),
            str(ROOT / "tests/test_crash_trace.c"), "-o", crash_binary,
        ], check=True)
        subprocess.run([crash_binary], check=True)
        session_shims = Path(directory) / "session"
        for name in ["esp_err.h", "esp_log.h", "esp_random.h", "esp_timer.h",
                     "esp_webrtc.h", "sdkconfig.h", "nvs.h", "freertos/FreeRTOS.h", "freertos/task.h"]:
            header = session_shims / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text(f'#include "{ROOT / "tests/session_platform.h"}"\n')
        session_binary = str(Path(directory) / "test_session")
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
            "-fsanitize=address,undefined", "-I", str(session_shims), "-I", str(ROOT / "main"),
            "-I", str(cjson), str(ROOT / "tests/test_session.c"),
            str(ROOT / "main/translation_protocol.c"), cjson_object, "-o", session_binary,
        ], check=True)
        subprocess.run([session_binary], check=True)
        usb_shims = Path(directory) / "usb"
        usb_shims.mkdir()
        # Reuse firmware channel counts, formats and sample rate with the real descriptor macros.
        config = (ROOT / "sdkconfig").read_text().splitlines()
        (usb_shims / "sdkconfig.h").write_text("\n".join(
            line for line in config if line.startswith("CONFIG_UAC_") or
            line.startswith("CONFIG_USB_DEVICE_UAC_AS_PART="))
            .replace("CONFIG_", "#define CONFIG_").replace("=y", " 1").replace("=", " ") + "\n")
        usb_binary = str(Path(directory) / "test_usb_descriptors")
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
            "-fsanitize=address,undefined", '-DCFG_TUSB_CONFIG_FILE="usb_config.h"',
            "-I", str(usb_shims), "-I", str(ROOT / "tests"), "-I", str(ROOT / "main"),
            "-I", str(ROOT / "managed_components/espressif__tinyusb/src"),
            "-I", str(ROOT / "managed_components/espressif__usb_device_uac/tusb_uac"),
            str(ROOT / "tests/test_usb_descriptors.c"), "-o", usb_binary,
        ], check=True)
        subprocess.run([usb_binary], check=True)
        overlay = usb_shims / "audio_device.c"
        subprocess.run([
            "python3", str(ROOT / "scripts/prepare_tinyusb_audio.py"),
            str(ROOT / "managed_components/espressif__tinyusb/src/class/audio/audio_device.c"),
            str(overlay),
        ], check=True)
        (usb_shims / "esp_timer.h").write_text("#include <stdint.h>\nint64_t esp_timer_get_time(void);\n")
        driver_binary = str(Path(directory) / "test_usb_audio_driver")
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
            "-fsanitize=address,undefined", '-DCFG_TUSB_CONFIG_FILE="usb_config.h"',
            "-I", str(usb_shims), "-I", str(ROOT / "tests"), "-I", str(ROOT / "main"),
            "-I", str(ROOT / "managed_components/espressif__tinyusb/src"),
            "-I", str(ROOT / "managed_components/espressif__tinyusb/src/class/audio"),
            "-I", str(ROOT / "managed_components/espressif__usb_device_uac/tusb_uac"),
            str(ROOT / "tests/test_usb_audio_driver.c"), str(overlay),
            str(ROOT / "main/usb_descriptors.c"), str(ROOT / "main/usb_uac_trace.c"),
            str(ROOT / "managed_components/espressif__tinyusb/src/common/tusb_fifo.c"),
            "-o", driver_binary,
        ], check=True)
        subprocess.run([driver_binary], check=True)


if __name__ == "__main__":
    main()
