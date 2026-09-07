#!/usr/bin/env python3
"""Scan an sd:/unicard.log trace (v2 format: "<kind> <A15-A8> <byte>").

Reports bus-level anomalies and a compact command flow:
  - C/W entries whose high byte differs from the data byte: the manager only
    writes with OUT (n),A, where A15-A8 == data, so a mismatch is a phantom
    capture (or a write from foreign code).
  - INIR runs of R/S reads whose B counter (high byte) skips a value: a read
    the device did not see (swallowed transaction) or one it saw twice.
  - status records that do not follow a STSR.
Usage: trace_check.py sd_unicard.log
"""
import sys

def main(path):
    ents = []
    for ln, line in enumerate(open(path, errors="replace"), 1):
        p = line.split()
        if len(p) != 3 or p[0] not in "CWRS":
            continue
        try:
            ents.append((ln, p[0], int(p[1], 16), int(p[2], 16)))
        except ValueError:
            continue
    print(f"{len(ents)} entries")

    # 1. phantom writes
    bad = [(ln, k, hi, d) for ln, k, hi, d in ents if k in "CW" and hi != d]
    print(f"writes with hi != data (phantom candidates): {len(bad)}")
    for ln, k, hi, d in bad[:40]:
        print(f"  line {ln}: {k} hi={hi:02X} data={d:02X}")

    # 2. INIR counter continuity within runs of the same kind
    gaps = 0
    i = 0
    while i < len(ents):
        j = i
        while j + 1 < len(ents) and ents[j + 1][1] == ents[i][1]:
            j += 1
        if ents[i][1] in "RS" and j - i >= 2:
            for k in range(i + 1, j + 1):
                exp = (ents[k - 1][2] - 1) & 0xFF
                if ents[k][2] != exp:
                    # a new INIR run legitimately restarts the counter; flag only
                    # skips inside a descending run (prev != 1 and not a restart at 0/len)
                    if ents[k - 1][2] != 1 and ents[k][2] not in (0,) and not (ents[k][2] > ents[k - 1][2]):
                        gaps += 1
                        if gaps <= 40:
                            print(f"  line {ents[k][0]}: {ents[k][1]} counter {ents[k-1][2]:02X} -> {ents[k][2]:02X} (expected {exp:02X})")
        i = j + 1
    print(f"counter skips inside read runs: {gaps}")

    # 3. compact flow
    print("\nflow:")
    i = 0
    out = []
    while i < len(ents):
        ln, k, hi, d = ents[i]
        if k == "C":
            params = []
            j = i + 1
            while j < len(ents) and ents[j][1] == "W":
                params.append(ents[j][3]); j += 1
            s = "".join(chr(b) if 32 <= b < 127 else f"\\x{b:02X}" for b in params)
            if d == 0x03:
                st = [e[3] for e in ents[j:j + 4] if e[1] == "S"]
                out.append(f"{ln}: STSR -> {' '.join(f'{b:02X}' for b in st)}")
                j += len(st)
            else:
                out.append(f"{ln}: C {d:02X} {s!r}" if params else f"{ln}: C {d:02X}")
            i = j
        else:
            j = i
            while j < len(ents) and ents[j][1] == k:
                j += 1
            out.append(f"{ln}: {k} x{j - i}")
            i = j
    # collapse repeated STSR polls
    last = None; rep = 0
    for o in out:
        key = o.split(": ", 1)[1]
        if key == last:
            rep += 1; continue
        if rep: print(f"    (x{rep + 1})")
        print("  " + o); last = key; rep = 0
    if rep: print(f"    (x{rep + 1})")

if __name__ == "__main__":
    main(sys.argv[1])
