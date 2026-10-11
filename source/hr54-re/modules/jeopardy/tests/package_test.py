#!/usr/bin/env python3
"""Packaging acceptance for the Jeopardy .rbox.

Checks the deterministic archive contents, member modes, entry count and size
limits, the manifest contract, the MIPS architecture of both executables, the
bundled default database magic and the executable entrypoint. Stdlib only."""
import gzip
import io
import json
import sys
import tarfile

CHECKS = 0
FAILS = 0


def check(cond, msg):
    global CHECKS, FAILS
    CHECKS += 1
    if not cond:
        FAILS += 1
        print("FAIL:", msg)


def elf_mips(data):
    if len(data) < 20 or data[:4] != b"\x7fELF":
        return False
    ei_class = data[4]      # 1 = 32-bit
    ei_data = data[5]       # 2 = big-endian
    e_machine = int.from_bytes(data[18:20], "big")
    return ei_class == 1 and ei_data == 2 and e_machine == 8  # EM_MIPS


def main(path):
    raw = open(path, "rb").read()
    check(len(raw) <= 64 * 1024 * 1024, "compressed package within 64 MiB")
    tar = tarfile.open(fileobj=io.BytesIO(gzip.decompress(raw)), mode="r:")
    members = {m.name: m for m in tar.getmembers()}
    check(len(members) <= 4096, "entry count within limit")
    total = sum(m.size for m in tar.getmembers())
    check(total <= 128 * 1024 * 1024, "unpacked size within limit")

    expected = {"module.json", "icon.png", "OFL.txt", "bin/module",
                "bin/hr54-jeopardy-native", "default-data/questions.jdb"}
    check(expected <= set(members), "archive carries all required members")
    check("bin/module" in members and members["bin/module"].mode == 0o700, "module executable mode 0700")
    check(members["bin/hr54-jeopardy-native"].mode == 0o700, "renderer executable mode 0700")
    check(members["default-data/questions.jdb"].mode == 0o600, "default database mode 0600")
    check(members["icon.png"].mode == 0o600, "icon mode 0600")
    check(members["OFL.txt"].mode == 0o600, "font license mode 0600")

    manifest = json.loads(tar.extractfile("module.json").read())
    check(manifest["id"] == "jeopardy", "module id")
    check(manifest["kind"] == "native-app" and manifest["moduleApi"] == 1, "native-app API 1")
    check(manifest["minReboxApi"] == 1, "minimum ReBox API")
    check(manifest["entrypoint"] == "bin/module", "entrypoint is bin/module")
    check(manifest.get("icon") == "icon.png", "manifest declares icon")

    module = tar.extractfile("bin/module").read()
    native = tar.extractfile("bin/hr54-jeopardy-native").read()
    check(elf_mips(module), "service is a MIPS32 big-endian ELF")
    check(elf_mips(native), "renderer is a MIPS32 big-endian ELF")
    check(not module.startswith(b"#!"), "entrypoint is an ELF, not a script (no shebang)")

    db = tar.extractfile("default-data/questions.jdb").read()
    check(db[:8] == b"JEPDB001", "bundled default database has JEPDB1 magic")
    check(len(db) > 64, "bundled default database is non-trivial")

    print(f"package: {CHECKS} checks, {FAILS} failures")
    return 1 if FAILS else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1]))
