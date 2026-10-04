#!/usr/bin/env python3
"""Offline verifier for HR54 sigtst plugin signatures.

The public material is extracted from the exact production sigtst binary. Its
multi-precision inputs and the 20-byte r/s values use little-endian byte order;
the SHA-1 digest is interpreted in normal big-endian order.
"""

import argparse
import hashlib
import re
from pathlib import Path


KEY_OFFSETS = (0x44F4, 0x4574, 0x45F4)


def leint(value):
    return int.from_bytes(value, "little")


def load_keys(sigtst):
    data = sigtst.read_bytes()
    ys = [leint(data[o:o + 128]) for o in KEY_OFFSETS]
    return ys, leint(data[0x4674:0x46F4]), leint(data[0x46F4:0x4708]), leint(data[0x4708:0x4788])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image", type=Path)
    ap.add_argument("signature", type=Path)
    ap.add_argument("--sigtst", required=True, type=Path)
    args = ap.parse_args()
    text = args.signature.read_text(errors="replace")
    match = re.search(r"^SIGNATURE = ([0-9A-Fa-f]{80})$", text, re.MULTILINE)
    if not match:
        raise SystemExit("no 80-hex-digit SIGNATURE field")
    size_match = re.search(r"^SIZE = (\d+)$", text, re.MULTILINE)
    raw = bytes.fromhex(match.group(1))
    r, s = leint(raw[:20]), leint(raw[20:])
    digest = hashlib.sha1(args.image.read_bytes()).digest()
    h = int.from_bytes(digest, "big")
    ys, p, q, g = load_keys(args.sigtst)
    valid = []
    for key_type, y in enumerate(ys):
        if not (0 < r < q and 0 < s < q):
            continue
        w = pow(s, -1, q)
        v = (pow(g, h * w % q, p) * pow(y, r * w % q, p) % p) % q
        if v == r:
            valid.append(key_type)
    declared = int(size_match.group(1)) if size_match else None
    print(f"sha1={digest.hex()}")
    print(f"actual_size={args.image.stat().st_size} declared_size={declared}")
    print("valid_key_types=" + (",".join(map(str, valid)) if valid else "none"))
    raise SystemExit(0 if valid and (declared is None or declared == args.image.stat().st_size) else 1)


if __name__ == "__main__":
    main()
