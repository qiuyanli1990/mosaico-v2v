#!/usr/bin/env python3
"""Prepare the pinned DWC2 IN retry fix without changing managed sources."""
import argparse
import hashlib
from pathlib import Path


def prepare(source: str) -> str:
    signature = "static void handle_incomplete_iso_in("
    if source.count(signature) != 1:
        raise ValueError("Pinned DWC2 IN retry function changed")
    start = source.index(signature)
    end = source.index("\n}", start) + 2
    function = source[start:end]
    if hashlib.sha256(function.encode()).hexdigest() != "f5538852fdc41ca299ffc74088659be8e61e15bd075155dd90b2f141af1dd14d":
        raise ValueError("Pinned DWC2 IN recovery changed; review the retry patch")
    original = "        deptsiz.packet_count    = tu_div_ceil(xfer->total_len, xfer->max_size);"
    replacement = original + "\n        // A zero-length IN retry still needs one USB transaction.\n" \
        "        if (deptsiz.packet_count == 0) deptsiz.packet_count = 1;"
    if function.count(original) != 1:
        raise ValueError("Pinned DWC2 retry packet count changed")
    return source[:start] + function.replace(original, replacement) + source[end:]


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
