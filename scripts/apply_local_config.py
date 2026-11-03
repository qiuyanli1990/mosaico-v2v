#!/usr/bin/env python3
"""Overlay only application settings, without printing credential values."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SETTING = re.compile(r"^(CONFIG_TRANSLATOR_[A-Z0-9_]+)=")
UNSET = re.compile(r"^# (CONFIG_TRANSLATOR_[A-Z0-9_]+) is not set$")


def main():
    local = ROOT / "sdkconfig.local"
    generated = ROOT / "sdkconfig"
    if not local.is_file() or not generated.is_file():
        return
    overrides = {}
    for line in local.read_text().splitlines():
        match = SETTING.match(line) or UNSET.match(line)
        if match:
            overrides[match.group(1)] = line
        elif line.strip() and not line.lstrip().startswith("#"):
            raise SystemExit("sdkconfig.local may only contain CONFIG_TRANSLATOR_* settings")
    lines = []
    for line in generated.read_text().splitlines():
        match = SETTING.match(line) or UNSET.match(line)
        if match and match.group(1) in overrides:
            line = overrides.pop(match.group(1))
        lines.append(line)
    lines.extend(overrides.values())
    updated = "\n".join(lines) + "\n"
    if updated != generated.read_text():
        generated.write_text(updated)


if __name__ == "__main__":
    main()

