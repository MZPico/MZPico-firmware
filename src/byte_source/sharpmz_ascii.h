/*
 * sharpmz_ascii.h - SharpMZ ASCII <-> ASCII character conversion
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

#ifndef SHARPMZ_ASCII_H
#define SHARPMZ_ASCII_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Convert one character from SharpMZ ASCII to ASCII */
extern uint8_t sharpmz_cnv_from(uint8_t c);

/* Convert one character from ASCII to SharpMZ ASCII */
extern uint8_t sharpmz_cnv_to(uint8_t c);

#ifdef __cplusplus
}
#endif

#endif /* SHARPMZ_ASCII_H */
