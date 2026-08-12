#!/usr/bin/env python3
"""RamDisk write/read volume stress test as an MZF.

Fills pages 0 and 1 completely (64KB each) with a rolling +7 pattern,
one writeAddress(B=block) per 256-byte block (so all 256 high-byte
values are exercised per page), then reads everything back and compares.

Output (via monitor 0x03C3, prints A as hex):
  55            test started
  00            page 0 verified
  01            page 1 verified
  CC            success, done (endless loop)
on mismatch:
  EE <block> <expected> <got>   then endless loop
Requires [ramdisk] size >= 131072. Runtime ~3s.
"""

RD_PAGE = 0xE9
RD_DATA = 0xEA
RD_ADDR = 0xEB
PRTA = 0x03C3
ORG = 0x1200

START_VAL = {0: 0x05, 1: 0x38}   # distinct per page so page aliasing is caught

code = bytearray()
labels = {}
fixups = []   # (position of 16-bit little-endian address, label name)

def label(name): labels[name] = len(code)
def di():           code.append(0xF3)
def xor_a():        code.append(0xAF)
def ld_a(n):        code.extend([0x3E, n])
def ld_d(n):        code.extend([0x16, n])
def ld_e(n):        code.extend([0x1E, n])
def ld_l(n):        code.extend([0x2E, n])
def ld_c(n):        code.extend([0x0E, n])
def ld_b_e():       code.append(0x43)
def ld_a_d():       code.append(0x7A)
def ld_a_e():       code.append(0x7B)
def ld_a_h():       code.append(0x7C)
def ld_d_a():       code.append(0x57)
def ld_h_a():       code.append(0x67)
def add_a(n):       code.extend([0xC6, n])
def cp_d():         code.append(0xBA)
def inc_l():        code.append(0x2C)
def inc_e():        code.append(0x1C)
def out_n_a(n):     code.extend([0xD3, n])
def out_c_a():      code.extend([0xED, 0x79])
def in_a(n):        code.extend([0xDB, n])
def call(nn):       code.extend([0xCD, nn & 0xFF, nn >> 8])
def jr_self():      code.extend([0x18, 0xFE])
def jp_nz(name):
    code.append(0xC2); fixups.append((len(code), name)); code.extend([0, 0])

def set_page(p):
    if p == 0: xor_a()
    else:      ld_a(p)
    out_n_a(RD_PAGE)

def print_a():      call(PRTA)

def set_block_addr():
    # pos = E<<8: B=E on A8-15, low byte 0
    ld_b_e(); ld_c(RD_ADDR); xor_a(); out_c_a()

di()
ld_a(0x55); print_a()                 # started marker

# ---- write phase: both pages fully ----
for p in (0, 1):
    set_page(p)
    ld_d(START_VAL[p])                # D = rolling pattern value
    ld_e(0)                           # E = block number (high addr byte)
    label(f"wblocks{p}")
    set_block_addr()
    ld_l(0)                           # L = byte-in-block counter
    label(f"wloop{p}")
    ld_a_d(); out_n_a(RD_DATA)        # write byte, auto-increment
    add_a(7); ld_d_a()                # next pattern value
    inc_l(); jp_nz(f"wloop{p}")       # 256 bytes per block
    inc_e(); jp_nz(f"wblocks{p}")     # 256 blocks = full 64KB page

# ---- verify phase ----
for p in (0, 1):
    set_page(p)
    ld_d(START_VAL[p])
    ld_e(0)
    label(f"rblocks{p}")
    set_block_addr()
    ld_l(0)
    label(f"rloop{p}")
    in_a(RD_DATA)                     # read byte, auto-increment
    cp_d(); jp_nz("fail")
    ld_a_d(); add_a(7); ld_d_a()
    inc_l(); jp_nz(f"rloop{p}")
    inc_e(); jp_nz(f"rblocks{p}")
    if p == 0: xor_a()
    else:      ld_a(p)
    print_a()                         # page verified

ld_a(0xCC); print_a()                 # success
jr_self()

label("fail")
ld_h_a()                              # save actual value
ld_a(0xEE); print_a()                 # failure marker
ld_a_e();   print_a()                 # block (= high addr byte)
ld_a_d();   print_a()                 # expected
ld_a_h();   print_a()                 # actual
jr_self()

# resolve jumps
for pos, name in fixups:
    addr = ORG + labels[name]
    code[pos] = addr & 0xFF
    code[pos + 1] = addr >> 8

body = bytes(code)

header = bytearray(128)
header[0] = 0x01
name = b"RDSTRESS"
header[1:1 + len(name)] = name
for i in range(1 + len(name), 0x12):
    header[i] = 0x0D
header[0x12:0x14] = len(body).to_bytes(2, "little")
header[0x14:0x16] = ORG.to_bytes(2, "little")
header[0x16:0x18] = ORG.to_bytes(2, "little")

import os
out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "rdstress.mzf")
with open(out_path, "wb") as f:
    f.write(bytes(header) + body)

print(f"wrote {out_path}: {len(body)} bytes of code, load/exec 0x{ORG:04X}")
print(body.hex(" "))
