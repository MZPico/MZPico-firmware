#!/usr/bin/env python3
"""Assemble the RamDisk high-address/paging test and wrap it as an MZF file.

Opcodes used (standard Z80 encodings):
  XOR A        AF
  OUT (n),A    D3 n
  LD B,n       06 n
  LD C,n       0E n
  OUT (C),A    ED 79
  LD A,n       3E n
  IN A,(n)     DB n
  CALL nn      CD lo hi
  RET          C9
"""

RD_PAGE = 0xE9
RD_DATA = 0xEA
RD_ADDR = 0xEB
PRTA = 0x03C3
ORG = 0x1200

code = bytearray()

def xor_a():        code.append(0xAF)
def out_n_a(n):     code.extend([0xD3, n])
def ld_b(n):        code.extend([0x06, n])
def ld_c(n):        code.extend([0x0E, n])
def out_c_a():      code.extend([0xED, 0x79])
def ld_a(n):        code.extend([0x3E, n])
def in_a(n):        code.extend([0xDB, n])
def call(nn):       code.extend([0xCD, nn & 0xFF, nn >> 8])
def in_a_c():       code.extend([0xED, 0x78])   # IN A,(C) - BC on address bus
def jr_self():      code.extend([0x18, 0xFE])   # JR $ - loop forever

def set_addr(high, low=0x00):
    """OUT (C),A with B=high drives A8-15 -> writeAddress high byte."""
    ld_b(high)
    ld_c(RD_ADDR)
    if low == 0:
        xor_a()
    else:
        ld_a(low)
    out_c_a()

def set_page(p):
    if p == 0:
        xor_a()
    else:
        ld_a(p)
    out_n_a(RD_PAGE)

def write_byte(val):
    ld_a(val)
    out_n_a(RD_DATA)

def read_and_print():
    in_a(RD_DATA)
    call(PRTA)          # prints A as hex

# ---- part 1: high-byte addressing inside page 0 (expect 11 22) ----
set_page(0)
set_addr(0x01); write_byte(0x11)    # page0:0100 <- 11
set_addr(0x02); write_byte(0x22)    # page0:0200 <- 22
set_addr(0x01); read_and_print()    # expect 11
set_addr(0x02); read_and_print()    # expect 22

# ---- part 2: page switching (expect 33 44) ----
set_page(0)
set_addr(0x00); write_byte(0x33)    # page0:0000 <- 33
set_page(1)
set_addr(0x00); write_byte(0x44)    # page1:0000 <- 44
set_page(0)
set_addr(0x00); read_and_print()    # expect 33
set_page(1)
set_addr(0x00); read_and_print()    # expect 44

# ---- part 3: READ-side high address byte (expect 5A A5) ----
# IN A,(C) puts B on A8-15; the ramdisk address port (0xEB) echoes it back.
for probe in (0x5A, 0xA5):
    ld_b(probe)
    ld_c(RD_ADDR)
    in_a_c()
    call(PRTA)

jr_self()

body = bytes(code)

# ---- MZF header: 128 bytes ----
header = bytearray(128)
header[0] = 0x01                          # attribute: machine code (OBJ)
name = b"RDTEST"
header[1:1 + len(name)] = name
for i in range(1 + len(name), 0x12):      # name field padded with 0x0D
    header[i] = 0x0D
header[0x12:0x14] = len(body).to_bytes(2, "little")   # file size
header[0x14:0x16] = ORG.to_bytes(2, "little")         # load address
header[0x16:0x18] = ORG.to_bytes(2, "little")         # exec address
# 0x18..0x7F stays zero (comment area)

import os
out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "rdtest.mzf")
with open(out_path, "wb") as f:
    f.write(bytes(header) + body)

print(f"wrote {out_path}: {len(body)} bytes of code, load/exec 0x{ORG:04X}")
print(body.hex(" "))
