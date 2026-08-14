/*
 * sharpmz_ascii.c - SharpMZ ASCII <-> ASCII character conversion
 *
 * Ported from mz800emu (src/libs/sharpmz_ascii) for MZPico.
 *
 * ----------------------------- License -------------------------------------
 *
 * GNU General Public License v3 (GPLv3)
 *
 * Copyright (C) 2017-2026 Michal Hucik <hucik@ordoz.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */

#include <stdint.h>

#include "sharpmz_ascii.h"

static const uint8_t sharpmz_ASCII_table[49] = {
    '_', ' ', 'e', ' ', '~', ' ', 't', 'g',
    'h', ' ', 'b', 'x', 'd', 'r', 'p', 'c',
    'q', 'a', 'z', 'w', 's', 'u', 'i', ' ',
    ' ', 'k', 'f', 'v', ' ', ' ', ' ', 'j',
    'n', ' ', ' ', 'm', ' ', ' ', ' ', 'o',
    'l', ' ', ' ', ' ', ' ', 'y', '{', ' ',
    '|'
};

uint8_t sharpmz_cnv_from(uint8_t c) {
    if (c <= 0x5d) return c;
    if (c == 0x80) return '}';
    if (c < 0x90 || c > 0xc0) return ' '; /* unknown characters become ' ' */
    return sharpmz_ASCII_table[c - 0x90];
}

uint8_t sharpmz_cnv_to(uint8_t c) {
    uint8_t i;

    if (c <= 0x5d) return c;
    if (c == '}') return 0x80;
    for (i = 0; i < sizeof(sharpmz_ASCII_table); i++) {
        if (c == sharpmz_ASCII_table[i]) {
            return i + 0x90;
        }
    }
    return ' '; /* unknown characters become ' ' */
}
