#!/usr/bin/env python3
"""Prepare an offline stock native-shell ownership policy for this HR54 build.

This changes only KeyEventTranslator's startup exclusive-key set. It does not
install, signal a process, alter the dispatcher, or write a vendor filesystem.
The receiver needs to reload the stock middleware before this takes effect.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "re"))
from car_classes import Car

STOCK_SHA256 = "3aa04459d3f0e69b8e9b9fc72638cc8a87b31ffa1a8eccd8bf98645bda35334c"
CLASS = "com/directv/mw/bsp/keydispatcher/KeyEventTranslator"
# One balanced TreeSet.add(Integer(MENU)) statement in InitTable. Keeping
# the bytecode length leaves every class/method/directory offset unchanged.
STATEMENT = bytes.fromhex("2ab40059bb0024591200b7004ab6005357")
# Verified Integer constants and complete balanced TreeSet.add statements in
# this exact InitTable method.  No parser or registration checks are changed.
RELEASE = {
    "GUIDE": (3549, 100, 0xE00B),
    "MENU":  (3566, 149, 0xE503),
    "LIST":  (3583, 146, 0xE501),
    "EXIT":  (3600, 148, 0xE502),
}


def prepare(source, destination):
    original = source.read_bytes()
    if hashlib.sha256(original).hexdigest() != STOCK_SHA256:
        raise ValueError("unsupported stock dtv.car; refusing a different build")
    if source.resolve() == destination.resolve():
        raise ValueError("output must be a separate offline copy")
    if destination.exists():
        raise ValueError("output already exists; refusing to overwrite it")
    car = Car(source)
    before = car.describe(CLASS)
    method = next(m for m in before["methods"] if m["name"] == "InitTable")
    code = bytes.fromhex(method["code"])
    pool = {p["index"]: p["resolved"] for p in before["constant_pool"]}
    if "m_setDruidExclusiveKeys" not in pool[89]:
        raise ValueError("unexpected stock policy field")
    changed = bytearray(original)
    ranges=[]
    for name,(offset,index,raw) in RELEASE.items():
        expected=bytearray(STATEMENT);expected[9]=index
        if pool.get(index)!=str(raw) or code[offset:offset+len(expected)]!=expected:
            raise ValueError(f"unexpected {name} policy statement")
        start=int(method["code_offset"],16)+offset
        changed[start:start+len(expected)]=bytes(len(expected));ranges.append((start,len(expected)))
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(changed)
    try:
        after = Car(destination).describe(CLASS)
        edited = next(m for m in after["methods"] if m["name"] == "InitTable")
        expected_code=bytearray(code)
        for offset,_,_ in RELEASE.values():expected_code[offset:offset+len(STATEMENT)]=bytes(len(STATEMENT))
        if edited["code"] != bytes(expected_code).hex():
            raise ValueError("policy bytecode verification failed")
        # Verify all class metadata and every other method are identical.
        edited["code"] = method["code"]
        expected_file=bytearray(original)
        for start,length in ranges:expected_file[start:start+length]=bytes(length)
        if after != before or changed != expected_file:
            raise ValueError("unexpected change outside MENU startup policy")
    except Exception:
        destination.unlink()
        raise
    return {
        "stock_sha256": STOCK_SHA256,
        "policy_sha256": hashlib.sha256(changed).hexdigest(),
        "bytes": len(changed),
        "class": CLASS,
        "method": "InitTable",
        "statements": {name:{"method_offset":hex(v[0]),"constant_pool":v[1],"raw":hex(v[2])} for name,v in RELEASE.items()},
        "statement_bytes": len(STATEMENT),
        "policy": "GUIDE, MENU, LIST and EXIT omitted from stock startup exclusive set",
        "installed": False,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(prepare(args.stock, args.output), indent=2))
    except (OSError, ValueError, StopIteration) as error:
        parser.exit(1, f"menu-policy: {error}\n")
