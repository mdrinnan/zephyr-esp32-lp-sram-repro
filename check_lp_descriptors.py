#!/usr/bin/env python3
"""Verify an MCUboot-format Espressif image loads every byte of LP-SRAM it needs to.

Reads the .metadata load header emitted by soc/espressif/*/default.ld and checks the two
LP descriptors against the ELF's actual .rtc.* PROGBITS sections. Fails if any loadable LP
section is uncovered, or if a descriptor's VMA span disagrees with its LMA span (which is
what happens when NOLOAD sections are interleaved between the loadable ones).

Usage: check_lp_descriptors.py <zephyr.elf>   ->  exit 0 pass, 1 fail
"""
import os
import re
import subprocess
import sys

if len(sys.argv) != 2:
    sys.exit(f"usage: {os.path.basename(sys.argv[0])} <zephyr.elf>   (needs readelf on PATH)")

ELF = sys.argv[1]

if not os.path.isfile(ELF):
    sys.exit(f"{ELF}: no such file")


def words(section):
    out = subprocess.run(["readelf", "-x", section, ELF],
                         capture_output=True, text=True).stdout
    vals = []
    for line in out.splitlines():
        m = re.match(r"\s+0x[0-9a-f]+((?:\s[0-9a-f]{8})+)", line)
        if m:
            for w in m.group(1).split():
                # readelf prints raw bytes in address order; the target is little-endian.
                vals.append(int.from_bytes(bytes.fromhex(w), "little"))
    return vals


def sections():
    out = subprocess.run(["readelf", "-S", "-W", ELF], capture_output=True, text=True).stdout
    secs = []
    for line in out.splitlines():
        m = re.search(r"\](\s+)(\.rtc[.\w]*)\s+(\w+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", line)
        if m and m.group(3) == "PROGBITS" and int(m.group(6), 16) > 0:
            secs.append({"name": m.group(2), "vma": int(m.group(4), 16),
                         "off": int(m.group(5), 16), "size": int(m.group(6), 16)})
    return sorted(secs, key=lambda s: s["vma"])


w = words(".metadata")
if not w or w[0] != 0xACE637D3:
    sys.exit(f"{ELF}: no valid .metadata load header (not an MCUboot build?)")

descs = [{"name": "LP_IRAM", "dst": w[8], "off": w[9], "size": w[10]},
         {"name": "LP_DRAM", "dst": w[11], "off": w[12], "size": w[13]}]
secs = [s for s in sections() if s["name"] != ".rtc_reserved"]

print(f"{ELF}")
for d in descs:
    print(f"  {d['name']:<8} dst=0x{d['dst']:08x} lma=0x{d['off']:06x} size=0x{d['size']:04x}")
for s in secs:
    print(f"  {s['name']:<18} vma=0x{s['vma']:08x} size=0x{s['size']:04x}")

fail = []

# 1. every loadable LP section must fall entirely inside exactly one descriptor
for s in secs:
    hits = [d for d in descs
            if d["size"] and d["dst"] <= s["vma"] and s["vma"] + s["size"] <= d["dst"] + d["size"]]
    if len(hits) != 1:
        fail.append(f"{s['name']} @0x{s['vma']:08x}+0x{s['size']:x} covered by {len(hits)} descriptors")
    else:
        s["desc"] = hits[0]

# 2. within a descriptor, VMA offset must equal LMA offset -- else the copy lands wrong
for d in descs:
    mine = [s for s in secs if s.get("desc") is d]
    if not mine:
        continue
    base_off = min(s["off"] for s in mine)
    if mine[0]["vma"] != d["dst"]:
        fail.append(f"{d['name']} starts at 0x{d['dst']:08x} but first section is 0x{mine[0]['vma']:08x}")
    for s in mine:
        if (s["vma"] - d["dst"]) != (s["off"] - base_off):
            fail.append(f"{s['name']}: VMA offset 0x{s['vma']-d['dst']:x} != LMA offset "
                        f"0x{s['off']-base_off:x} in {d['name']} (interleaved NOLOAD section)")
    span = max(s["vma"] + s["size"] for s in mine) - d["dst"]
    if span > d["size"]:
        fail.append(f"{d['name']} size 0x{d['size']:x} < span 0x{span:x} needed")

if fail:
    print("  FAIL")
    for f in fail:
        print(f"    - {f}")
    sys.exit(1)
print(f"  PASS  ({len(secs)} LP sections fully covered)")
