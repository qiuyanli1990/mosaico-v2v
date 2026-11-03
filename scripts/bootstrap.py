#!/usr/bin/env python3
"""Fetch pinned dependencies without changing other ESP-IDF installations."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DEPENDENCIES = (
    ("third_party/esp-webrtc-solution", "https://github.com/coin0/esp-webrtc-solution.git",
     "b57d626c9a885c75a216a5b2deccc6bc2fe2e045"),
    ("third_party/esp-mosaico-bsp", "https://github.com/esp-mosaico/esp-mosaico-bsp.git",
     "a7a93d290be28d18fbfd32361c90aa20b0cd1c67"),
)
IDF = (".tools/esp-idf", "https://github.com/espressif/esp-idf.git",
       "7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe")
IDF_SUBMODULES = (
    "components/bootloader/subproject/components/micro-ecc/micro-ecc",
    "components/spiffs/spiffs",
    "components/mbedtls/mbedtls", "components/lwip/lwip", "components/esp_wifi/lib",
    "components/esp_phy/lib", "components/esp_coex/lib", "components/heap/tlsf",
)


def run(*args, env=None):
    subprocess.run(args, cwd=ROOT, env=env, check=True)


def checkout(spec):
    relative, remote, commit = spec
    path = ROOT / relative
    if not (path / ".git").exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        run("git", "init", str(path))
        run("git", "-C", str(path), "remote", "add", "origin", remote)
        run("git", "-C", str(path), "fetch", "--depth", "1", "origin", commit)
        run("git", "-C", str(path), "checkout", "--detach", commit)
    current = subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
    if current != commit:
        raise RuntimeError(f"{relative} is at {current}, expected {commit}; leaving it unchanged")


def patch_render_codec_requirement():
    # Both implementations use the 1.x esp_codec_dev API; BSP/capture require 1.x.
    path = ROOT / "third_party/esp-webrtc-solution/components/av_render/idf_component.yml"
    source = path.read_text()
    old = 'espressif/esp_codec_dev:  "^2.0.0-beta1"'
    new = 'espressif/esp_codec_dev:  "~1.6"'
    if old in source:
        path.write_text(source.replace(old, new))
    elif new not in source:
        raise RuntimeError("Unexpected av_render manifest; refusing to patch")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install-idf", action="store_true")
    args = parser.parse_args()
    for dependency in DEPENDENCIES:
        checkout(dependency)
    patch_render_codec_requirement()
    if args.install_idf:
        checkout(IDF)
        run("git", "-C", str(ROOT / IDF[0]), "submodule", "update", "--init", "--recursive",
            "--depth", "1", *IDF_SUBMODULES)
        env = dict(os.environ, IDF_TOOLS_PATH=str(ROOT / ".tools/espressif"))
        tools = ROOT / IDF[0] / "tools/idf_tools.py"
        run(sys.executable, str(tools), "--non-interactive", "install", "riscv32-esp-elf",
            "esp-rom-elfs", "esp-idf-configdep", env=env)
        run(sys.executable, str(tools), "--non-interactive", "install-python-env", env=env)
    print("Pinned dependencies are ready.")


if __name__ == "__main__":
    main()

