#ifndef __MZ800PICO_MANAGER_CONSOLE_H__
#define __MZ800PICO_MANAGER_CONSOLE_H__

#include <stdint.h>

void put_str_xy(uint8_t x, uint8_t y, char *s);
void put_str_attr_xy(uint8_t x, uint8_t y, char *s, uint8_t attr);
void put_char_attr_xy(uint8_t x, uint8_t y, char c, uint8_t attr);
void put_multi_char_xy(uint8_t x, uint8_t y, uint8_t c, uint8_t cnt);
void put_multi_attr_xy(uint8_t x, uint8_t y, uint8_t attr, uint8_t cnt);
void border(uint8_t color);
void clrscr(void);
void scroll_down(void);
void scroll_up(void);
uint8_t inkey(void);

#endif
