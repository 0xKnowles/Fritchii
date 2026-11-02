#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Disassemble one synthetic native instruction with NVIDIA's nvdisasm."""

import argparse
from pathlib import Path
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("nvdisasm", type=Path)
    parser.add_argument("words", nargs=4, type=lambda word: int(word, 16))
    parser.add_argument("--arch", default="SM87")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="eglantine-instruction-") as directory:
        binary = Path(directory) / "instruction.bin"
        binary.write_bytes(struct.pack("<4I", *args.words))
        subprocess.run([str(args.nvdisasm), "-b", args.arch, "-ndf", str(binary)], check=True)


if __name__ == "__main__":
    main()
