#pragma once
#include <cstdint>
// A realistic MZF: 128-byte header (attr, name, body size at 18-19, load 20-21,
// exec 22-23) followed by a 300-byte body with a recognisable pattern.
static const uint8_t mzf_menu[128 + 300] = {
    0x01, 'M','E','N','U',0x0d,0x0d,0x0d,0x0d,0x0d,0x0d,0x0d,0x0d,0x0d,0x0d,0x0d,0x0d,0x0d,
    0x2c, 0x01,   // body size 300
    0x00, 0x12,   // load 0x1200
    0x00, 0x12,   // exec 0x1200
    // rest of header zero-filled by the initializer
};
static const uint8_t mzf_explorer[] = {0x01, 'E', 'X', 'P'};
// The real 42 KB BASIC image: the loader-sequence test streams it in full
#include "../../../src/mzf_basic.hpp"
