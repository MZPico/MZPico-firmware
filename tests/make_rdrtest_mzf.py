#!/usr/bin/env python3
"""RamDisk READ-path error-rate instrument as an MZF.

Purpose: turn the binary "boot checksum ok/failed" oracle into a measurable
error rate for A/B testing bus-timing firmware variants (16-bit read
experiments). Writes page 0 (64KB) once with a rolling +7 pattern using only
auto-increment writes (deliberately independent of 16-bit addressing), then
runs 16 read passes of the full page (1M reads total, ~16s), counting
mismatches in BC instead of stopping.

Output (via monitor 0x03C3, prints A as hex):
  55                            test started
  <pass> <errhi> <errlo>        after each pass: pass index + RUNNING total
  CC <errhi> <errlo>            done: 16-bit total mismatch count
  [EE <pass> <block> <byte> <expected> <got>]  first-error detail, if any
then endless loop. Per-pass deltas of the running total show whether errors
are uniform or bursty.

Works with default [ramdisk] size (65536); uses only ports 0xF8/0xEA.
"""

RD_DATA = 0xEA
RD_RESET = 0xF8   # read port: resets ramdisk position counter
PRTA = 0x03C3
ORG = 0x1200
PASSES = 16
START_VAL = 0x05

code = bytearray()
labels = {}
fixups = []   # (position of 16-bit little-endian address, label name)

def label(name): labels[name] = len(code)
def di():           code.append(0xF3)
def xor_a():        code.append(0xAF)
def or_a():         code.append(0xB7)
def ld_a(n):        code.extend([0x3E, n])
def ld_d(n):        code.extend([0x16, n])
def ld_e(n):        code.extend([0x1E, n])
def ld_h(n):        code.extend([0x26, n])
def ld_l(n):        code.extend([0x2E, n])
def ld_bc(nn):      code.extend([0x01, nn & 0xFF, nn >> 8])
def ld_a_b():       code.append(0x78)
def ld_a_c():       code.append(0x79)
def ld_a_d():       code.append(0x7A)
def ld_a_e():       code.append(0x7B)
def ld_a_h():       code.append(0x7C)
def ld_a_l():       code.append(0x7D)
def ld_d_a():       code.append(0x57)
def add_a(n):       code.extend([0xC6, n])
def cp_d():         code.append(0xBA)
def cp_n(n):        code.extend([0xFE, n])
def inc_l():        code.append(0x2C)
def inc_e():        code.append(0x1C)
def inc_h():        code.append(0x24)
def inc_bc():       code.append(0x03)
def out_n_a(n):     code.extend([0xD3, n])
def in_a(n):        code.extend([0xDB, n])
def push_af():      code.append(0xF5)
def pop_af():       code.append(0xF1)
def jr_self():      code.extend([0x18, 0xFE])
def db(n):          code.append(n)

def _abs16(opcode, name):
    code.append(opcode); fixups.append((len(code), name)); code.extend([0, 0])
def jp(name):        _abs16(0xC3, name)
def jp_nz(name):     _abs16(0xC2, name)
def jp_z(name):      _abs16(0xCA, name)
def ld_a_mem(name):  _abs16(0x3A, name)
def ld_mem_a(name):  _abs16(0x32, name)

def print_a_saved():
    # monitor register preservation is unverified; save what we rely on
    code.append(0xC5)  # PUSH BC
    code.append(0xE5)  # PUSH HL
    code.extend([0xCD, PRTA & 0xFF, PRTA >> 8])
    code.append(0xE1)  # POP HL
    code.append(0xC1)  # POP BC

di()
ld_a(0x55); print_a_saved()           # started marker

# ---- write phase: fill page 0 via auto-increment only ----
in_a(RD_RESET)                        # position := 0
ld_d(START_VAL)                       # D = rolling pattern value
ld_e(0)                               # E = block counter
label("wblocks")
ld_l(0)                               # L = byte-in-block counter
label("wloop")
ld_a_d(); out_n_a(RD_DATA)            # write byte, auto-increment
add_a(7); ld_d_a()
inc_l(); jp_nz("wloop")               # 256 bytes per block
inc_e(); jp_nz("wblocks")             # 256 blocks = 64KB

# ---- read passes, counting mismatches ----
ld_h(0)                               # H = pass counter
ld_bc(0)                              # BC = total mismatch count
label("passloop")
in_a(RD_RESET)                        # position := 0
ld_d(START_VAL)
ld_e(0)
label("rblocks")
ld_l(0)
label("rloop")
in_a(RD_DATA)                         # read byte, auto-increment
cp_d(); jp_nz("mism")
label("rback")
ld_a_d(); add_a(7); ld_d_a()
inc_l(); jp_nz("rloop")
inc_e(); jp_nz("rblocks")
ld_a_h(); print_a_saved()             # pass index
ld_a_b(); print_a_saved()             # running total high
ld_a_c(); print_a_saved()             # running total low
inc_h()
ld_a_h(); cp_n(PASSES); jp_nz("passloop")

# ---- report ----
ld_a(0xCC); print_a_saved()
ld_a_b(); print_a_saved()             # error count high
ld_a_c(); print_a_saved()             # error count low
ld_a_mem("fflag"); or_a(); jp_z("done")
ld_a(0xEE); print_a_saved()
for f in ("fpass", "fblock", "fbyte", "fexp", "fgot"):
    ld_a_mem(f); print_a_saved()
label("done")
jr_self()

# ---- mismatch: count, record first occurrence ----
label("mism")
inc_bc()                              # INC BC preserves flags/regs
push_af()                             # A = actual (got) value
ld_a_mem("fflag"); or_a(); jp_nz("mskip")
ld_a(1);  ld_mem_a("fflag")
ld_a_h(); ld_mem_a("fpass")
ld_a_e(); ld_mem_a("fblock")
ld_a_l(); ld_mem_a("fbyte")
ld_a_d(); ld_mem_a("fexp")
pop_af(); ld_mem_a("fgot")
jp("rback")
label("mskip")
pop_af()
jp("rback")

# ---- data ----
for f in ("fflag", "fpass", "fblock", "fbyte", "fexp", "fgot"):
    label(f); db(0)

for pos, name in fixups:
    addr = ORG + labels[name]
    code[pos] = addr & 0xFF
    code[pos + 1] = addr >> 8

body = bytes(code)

header = bytearray(128)
header[0] = 0x01
name = b"RDRTEST"
header[1:1 + len(name)] = name
for i in range(1 + len(name), 0x12):
    header[i] = 0x0D
header[0x12:0x14] = len(body).to_bytes(2, "little")
header[0x14:0x16] = ORG.to_bytes(2, "little")
header[0x16:0x18] = ORG.to_bytes(2, "little")

import os
out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "rdrtest.mzf")
with open(out_path, "wb") as f:
    f.write(bytes(header) + body)

print(f"wrote {out_path}: {len(body)} bytes of code, load/exec 0x{ORG:04X}")
print(body.hex(" "))
