#!/usr/bin/env python3
"""Exercise actual LVGL navigation and render the v6 pages on the host."""
import argparse
from pathlib import Path
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[1]


def save_png(path, width, height, rgb):
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

    rows = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3]
                    for y in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" +
                     chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def create_previews(output):
    renders = sorted(output.glob("[0-9][0-9]-*.ppm"))
    width = 3 * 492 + 12
    height = ((len(renders) + 2) // 3) * 492 + 12
    overview = bytearray(b"\xee\xe9\xdf" * width * height)
    for index, path in enumerate(renders):
        header, dimensions, maximum, rgb = path.read_bytes().split(b"\n", 3)
        if (header, dimensions, maximum) != (b"P6", b"480 480", b"255") or len(rgb) != 480 * 480 * 3:
            raise ValueError(f"Unexpected LVGL render format: {path}")
        save_png(path.with_suffix(".png"), 480, 480, rgb)
        x, y = 12 + (index % 3) * 492, 12 + (index // 3) * 492
        for row in range(480):
            start = ((y + row) * width + x) * 3
            overview[start:start + 480 * 3] = rgb[row * 480 * 3:(row + 1) * 480 * 3]
    save_png(output / "v6-overview.png", width, height, overview)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/ui-preview-v6")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    host_build = ROOT / ".tools/ui-host-v6"
    subprocess.run(["cmake", "-S", str(ROOT / "tests/ui"), "-B", str(host_build),
                    "-G", "Ninja"], check=True)
    subprocess.run(["cmake", "--build", str(host_build), "--parallel", "8"], check=True)
    subprocess.run([str(host_build / "ui_test"), str(output)], check=True)
    create_previews(output)
    print(f"LVGL page renders (PNG + PPM): {output}")
    print(f"Page overview: {output / 'v6-overview.png'}")


if __name__ == "__main__":
    main()
