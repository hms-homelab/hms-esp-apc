#!/usr/bin/env python3
"""
Turn a captured /hid page into inputs for hid_replay.c.

Usage:  python3 extract_hid.py hid_capture.txt outdir/

Writes  outdir/descriptor.bin   raw HID report descriptor
        outdir/reports.txt      "<type> <id> <payload hex>" per line, payload
                                EXCLUDING the leading report ID byte

The point is to replay a real device's own bytes through hid_pdc + ups_map on
a host, and compare against what that same board's hand-written parser printed
in its DECODED section. No board or ESP-IDF needed to run the comparison.
"""
import re
import sys
import pathlib


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    text = pathlib.Path(sys.argv[1]).read_text(errors="replace")
    out = pathlib.Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)

    lines = text.splitlines()

    # ── descriptor: hexdump lines "0000  05 84 09 ..." until a blank/# line ──
    desc = bytearray()
    in_desc = False
    for ln in lines:
        if ln.startswith("# HID REPORT DESCRIPTOR"):
            in_desc = True
            continue
        if in_desc:
            if not ln.strip() or ln.startswith("#"):
                break
            m = re.match(r"^\s*([0-9A-Fa-f]{4})\s+((?:[0-9A-Fa-f]{2}\s+)+)$", ln)
            if not m:
                continue
            desc += bytes(int(b, 16) for b in m.group(2).split())

    # ── raw reports: "  0C  INT   10  24  2.2  0  0C 62 4B 49 ..." ──
    reports = []
    for ln in lines:
        m = re.match(
            r"^\s*([0-9A-Fa-f]{2})\s+(INT|FEAT)\s+\d+\s+\d+\s+[\d.]+\s+\d+\s+((?:[0-9A-Fa-f]{2}\s*)+)$",
            ln,
        )
        if not m:
            continue
        rid = int(m.group(1), 16)
        rtype = m.group(2)
        payload = [int(b, 16) for b in m.group(3).split()]
        # First byte is the report ID; hid_pdc bit offsets are relative to
        # what follows it.
        if payload and payload[0] == rid:
            payload = payload[1:]
        reports.append((rtype, rid, payload))

    (out / "descriptor.bin").write_bytes(bytes(desc))
    with (out / "reports.txt").open("w") as fh:
        for rtype, rid, payload in reports:
            fh.write(f"{rtype} {rid:02X} {' '.join(f'{b:02X}' for b in payload)}\n")

    print(f"descriptor: {len(desc)} bytes -> {out/'descriptor.bin'}")
    print(f"reports:    {len(reports)} -> {out/'reports.txt'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
