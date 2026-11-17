#pragma once

// The PicoRD RAM disk's address logic ([pico_rd], pico_rd.cpp), apart from
// its storage so tests/cow_ramdisk can drive it on the host.
//
// One 24-bit position register:
//  - A2 / A1 / A0 (ports base+2..base+4): write replaces one byte of it and
//    keeps the other two; read returns that byte;
//  - ADDRS (base+5): writes A0, A1, A2 in turn (the byte index restarts at
//    A0 after A2, and on any other port access except ADDRI);
//  - ADDRI (base+6): skips forward by the byte written, wrapping at size;
//  - control (base+0, read or write): position 0, the ADDRS index too;
//  - data (base+1): the byte at the position, which then advances and wraps
//    at size.
// The register may hold a value at or past size (the bytes are written one
// at a time); a data access or a skip reduces it modulo size first. (Until
// 2026-11, A2 ORed into byte 2 and A1 cleared byte 2 while keeping byte 1;
// and a file-backed disk refused a seek past size, so the next byte write
// composed on the old position.)

#include <stdint.h>
#include "common.hpp"

constexpr uint32_t PICO_RD_ADDR_SPAN = 1u << 24;   // what 24 address bits reach

struct PicoRdAddr {
    uint32_t pos = 0;      // the register, 24 bits
    uint32_t size = 1;     // the bound: bytes, 1..2^24 (setSize)
    uint8_t  idx = 0;      // ADDRS: the next byte to write (0 = A0)

    // The bound: the configured size, else (size= omitted, file-backed) the
    // storage's own length; at most 2^24 - the register cannot address more,
    // so the position never leaves its 24 bits
    void setSize(uint32_t cfg_size, uint32_t storage_size = 0) {
        uint32_t s = cfg_size ? cfg_size : storage_size;
        if (s == 0) s = 1;
        if (s > PICO_RD_ADDR_SPAN) s = PICO_RD_ADDR_SPAN;
        size = s;
    }

    ALWAYS_INLINE void control() { pos = 0; idx = 0; }
    // Direct byte write, b = 0 (A0), 1 (A1), 2 (A2)
    ALWAYS_INLINE void setByte(uint32_t b, uint8_t v) {
        pos = ((pos & ~(0xFFu << (8 * b))) | ((uint32_t)v << (8 * b))) & 0xFFFFFFu;
        idx = 0;
    }
    ALWAYS_INLINE uint8_t getByte(uint32_t b) {
        idx = 0;
        return (uint8_t)(pos >> (8 * b));
    }
    ALWAYS_INLINE void addrs(uint8_t v) {
        pos = ((pos & ~(0xFFu << (8 * idx))) | ((uint32_t)v << (8 * idx))) & 0xFFFFFFu;
        idx = (uint8_t)(idx >= 2 ? 0 : idx + 1);
    }
    ALWAYS_INLINE void addri(uint8_t v) { pos = (cur() + v) % size; }
    // The position for a data access (reduced below size), then advanced
    ALWAYS_INLINE uint32_t take() {
        const uint32_t p = cur();
        pos = (p + 1 >= size) ? 0 : p + 1;
        idx = 0;
        return p;
    }
    ALWAYS_INLINE uint32_t cur() {
        if (__builtin_expect(pos >= size, 0)) pos %= size;
        return pos;
    }
};
