#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <input.h>
#include <string.h>

#include "mz800pico_manager_console.h"


uint8_t vram_codes[256] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
  0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
  0x00, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6b, 0x6a, 0x2f, 0x2a, 0x2e, 0x2d,
  0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x4f, 0x2c, 0x51, 0x2b, 0x57, 0x49,
  0x55, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
  0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x52, 0x59, 0x54, 0xbe, 0x7c,
  0xa4, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
  0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xac, 0x79, 0x40, 0xa5, 0x0f,
  0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
  0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
  0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
  0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf,
  0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf,
  0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
  0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
  0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0x6a, 0xfb, 0x6c, 0x4d, 0x4e, 0xff,
};

/* multiplication by 40
 * to be called from assembly code
 * inputs: 
 *    e - a number to multiply
 * outputs:
 *   hl - result
 * destroys:
 *   de
 *   hl
 *   f
 */
void _multiply40(void) __naked {
  __asm
    ld d, 0
    ld h, e
    sla e
    sla e
    sla e
    rl d
    sla e
    rl d
    sla e
    rl d

    ld l, h
    ld h, 0
    sla l
    sla l
    sla l

    add hl, de
    ret
  __endasm;
}

void put_str_xy(uint8_t x, uint8_t y, char *s) __naked {
    __asm
        push iy
        ld iy, 4
        add iy, sp

        ld e, (iy+2)    ; line number
        call __multiply40
        ld e, (iy+4)
        ld d, 0xd0
        add hl, de
        ld de, hl

        ld l, (iy+0)
        ld h, (iy+1)
insert_loop_psx:
        ld a, (hl)
        or a
        jr z, done_psx
        push hl
        ld c, a
        ld hl, _vram_codes
        ld b, 0
        add hl, bc
        ld a, (hl)
        ld (de), a
        inc de
        pop hl
        inc hl
        jr insert_loop_psx
done_psx:
        pop iy
        ret
    __endasm;
}

void put_str_attr_xy(uint8_t x, uint8_t y, char *s, uint8_t attr) __naked {
    __asm
        push iy
        ld iy, 4
        add iy, sp

        ld e, (iy+4)    ; line number
        call __multiply40
        ld e, (iy+6)
        ld d, 0xd0
        add hl, de
        ld de, hl

        ld l, (iy+2)
        ld h, (iy+3)
insert_loop_psa:
        ld a, (hl)
        or a
        jr z, done_psa

        push hl
        ld c, a
        ld hl, _vram_codes
        ld b, 0
        add hl, bc
        ld a, (hl)
        ld (de), a

        push de
        ld a, 8
        add a, d
        ld d, a
        ld a, (iy+0)
        ld (de), a
        pop de

        inc de
        pop hl
        inc hl
        jr insert_loop_psa
done_psa:
        pop iy
        ret
    __endasm;
}

void put_char_attr_xy(uint8_t x, uint8_t y, char c, uint8_t attr) __naked {
    __asm
        push iy
        ld iy, 4
        add iy, sp

        ld e, (iy+4)    ; y
        call __multiply40
        ld d, 0xD0
        ld e, (iy+6)    ; x
        add hl, de

        ld c, (iy+2)    ; char
        ld b, 0
        ld de, hl
        ld hl, _vram_codes
        add hl, bc
        ld a, (hl)
        ld hl, de

        ld (hl), a
        add hl, 0x0800
        ld a, (iy+0)    ; attr
        ld (hl), a

        pop iy
        ret
    __endasm;
}

void put_multi_char_xy(uint8_t x, uint8_t y, uint8_t c, uint8_t cnt) __naked {
    __asm
        push iy
        ld iy, 4
        add iy, sp

        ld e, (iy+4)    ; y
        call __multiply40
        ld e, (iy+6)    ; x
        ld d, 0xD0      ; d8000 - start of attribute vram
        add hl, de

        ld de, hl
        inc de          ; setup for ldir

        ld c, (iy+2)
        ld b, 0
        push hl
        ld hl, _vram_codes
        add hl, bc
        ld a, (hl)
        pop hl
        ld (hl), a      ; 1st attr
        ld c, (iy+0)    ; cnt
        dec c
        ld b, 0
        ldir            ; copy from the 1st attr on
        pop iy
        ret
    __endasm;
}

void put_multi_attr_xy(uint8_t x, uint8_t y, uint8_t attr, uint8_t cnt) __naked {
    __asm
        push iy
        ld iy, 4
        add iy, sp

        ld e, (iy+4)    ; y
        call __multiply40
        ld e, (iy+6)    ; x
        ld d, 0xD8      ; d8000 - start of attribute vram
        add hl, de

        ld de, hl
        inc de          ; setup for ldir

        ld c, (iy+2)
        ld (hl), c      ; 1st attr
        ld c, (iy+0)    ; cnt
        dec c
        ld b, 0
        ldir            ; copy from the 1st attr on
        pop iy
        ret
    __endasm;
}

void clrscr(void) __naked {
    __asm
        ld hl, 0xd000
        ld de, hl
        inc de
        xor a
        ld (hl), a
        ld bc, 0x0800-1
        ldir
        ld a, 0x71
        ld (hl), a
        ld bc, 0x800-1
        ldir
        ret
    __endasm;
}

void scroll_down(void) __naked {
    __asm
        ld hl, 0xd000 + 22*40 -1
        ld de, 0xd000 + 23*40 -1
        ld bc, 20*40
scroll_down_loop:
        ld a, (hl)
        ld (de), a
        dec hl
        dec de
        dec bc
        ld a,b
        or c
        jr nz, scroll_down_loop
        ret
    __endasm;
}

void scroll_up(void) __naked {
    __asm
       ld hl, 0xd000 + 3*40
       ld de, 0xd000 + 2*40
       ld bc, 20*40
scroll_up_loop:
       ld a, (hl)
       ld (de), a
       inc hl
       inc de
       dec bc
       ld a, b
       or c
       jr nz, scroll_up_loop
       ret
    __endasm;
}

void border(uint8_t color) __naked {
  __asm
        push iy
        ld iy, 4
        add iy, sp

        ld bc, 0x06cf
        ld a, (iy+0)
        out (c), a

        pop iy
        ret
  __endasm;
}

uint8_t inkey(void) {
  static uint8_t autorepeat_trigger;
  static uint8_t autorepeat_speed;
  static uint8_t curr_key;
  uint8_t c;

  c = getk();
  if (c != 0) {
    if (curr_key == c) {
      if (autorepeat_trigger <= 50) {
        autorepeat_trigger++;
        autorepeat_speed = 0;
        return 0;
      } else {
        autorepeat_speed++;
        if (autorepeat_speed <= 5)
          return 0;
        else
          autorepeat_speed = 0;
      }
    }
  } else
    autorepeat_trigger = 0;
  curr_key = c;
  return c;
}


