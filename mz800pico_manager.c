#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <input.h>
#include <string.h>

#include "mz800pico_manager_console.h"

typedef struct {
  char isDir;
  char filename[32];
  uint32_t size;
} DIR_ENTRY;

DIR_ENTRY entries[930];
#define VISIBLE_ENTRIES 21
uint16_t dir_items;
uint16_t file_selected;
uint16_t file_offset;
char path[255];

void read_and_execute(void) __naked {
  __asm
    PUBLIC _read_and_execute_start
    PUBLIC _read_and_execute_end
_read_and_execute_start:
    call 0xe729     ; header crc check
    ret nz          ; header crc failed
    in a, (c)
    inc c
    ld hl, 0x1102   ; start of working area
    ld b, 9         ; 9 bytes in header
    inir
    ld de, (0x1102) ; file size
    ld hl, 0x1200
    ld a,e
    or a
    jr z, no_part
    ld b,a
inir_loop:
    inir
no_part:
    ld b,0
    ld a, d
    or a
    jr z, inir_exit
    dec d
    jr inir_loop
inir_exit:
    ld de, 0x1200
    ld bc, (0x1102)
    call 0xe70e     ; rdcrc
    ld de, (0x1108)
    or a
    sbc hl, de
    jp nz, 0xeb24        ; body crc failed
    ld bc, 0
    exx
    ld hl, 0x1102
    jp 0xecfc     ; relocate and execute
_read_and_execute_end:
  __endasm;
}

extern uint8_t read_and_execute_start[];
extern uint8_t read_and_execute_end[];

void relocate_and_execute(void) {
    uint8_t *dst = (uint8_t *)0x1108;
    uint8_t *src = read_and_execute_start;
    uint16_t size = read_and_execute_end - read_and_execute_start;

    memcpy(dst, src, size);

    void (*relocated_read_and_execute)(void) = (void (*)(void))dst;
__asm
    ld c, 0x46
__endasm
    relocated_read_and_execute();
}


void exec_command(uint8_t command) __naked {
  __asm
    pop hl
    pop de
    ld a, e
    out (0x40), a
    push de
    push hl
    ret
  __endasm;
}

void set_command_data(uint8_t data) __naked {
  __asm
    pop hl
    pop de
    ld a, e
    out (0x41), a
    push de
    push hl
    ret
  __endasm;
}

uint8_t get_command_status(void) {
  __asm
    in a, (0x40)
    ld h, 0
    ld l, a
  __endasm;
}


uint8_t get_command_data(void) {
  __asm
    in a, (0x41)
    ld h, 0
    ld l, a
  __endasm;
}

uint8_t reset_command_data(void) {
  __asm
    in a, (0x42)
  __endasm;
}
void set_command_path(char *dir) {
  reset_command_data();
  for (uint16_t i=0; dir[i] != 0; i++)
    set_command_data(dir[i]);
  set_command_data(0);
}

void read_dir(char *path) {
#ifdef MZ800PICO_TEST
  dir_items = 0;
  entries[dir_items].isDir = 0;
  entries[dir_items].size = 15321;
  strcpy(entries[dir_items++].filename, "MZ1Z016.MZF");
  entries[dir_items].isDir = 0;
  entries[dir_items].size = 43721;
  strcpy(entries[dir_items++].filename, "FLAPPY.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "NAKAMOTO.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "THEDROP.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE1.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE2.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE3.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE4.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE5.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE6.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE7.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE8.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE9.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE10.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE11.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE12.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE13.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE14.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE15.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE16.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE17.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE18.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE19.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE20.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE21.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE22.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE23.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE24.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE25.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE26.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE27.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE28.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE29.MZF");
  entries[dir_items].isDir = 0;
  strcpy(entries[dir_items++].filename, "SUBMARINE30.MZF");
#else
  uint16_t i;
  uint8_t j;
  uint8_t *dst;
  set_command_path(path);
  exec_command(1);
  while (get_command_status() != 0x03);
  dir_items = get_command_data();
  dir_items += get_command_data() <<8;
  for (i=0; i<dir_items; i++) {
    dst = (uint8_t*)entries[i];
    for (j=0; j<sizeof(DIR_ENTRY); j++)
      *dst++ = get_command_data();
  }
#endif
}


void u32toa(uint32_t value, char *str) {
    char buf[11];
    uint8_t i = 0;
    if (value == 0) {
        *str++ = '0';
        *str = 0;
        return;
    }
    while (value > 0) {
        buf[i++] = '0' + (value % 10);
        value /= 10;
    }
    while (i > 0) {
        *str++ = buf[--i];
    }
    *str = 0;
}

void display_item(uint8_t index, uint8_t screen_line) {
    DIR_ENTRY *entry = &entries[index];
    char fileinfo[40];
    char *p = fileinfo;

    const char *name = entry->filename;
    uint8_t name_len = 0;

    // Estimate how many chars the size field will take
    uint8_t size_field_len;
    char size_str[10];

    if (entry->isDir) {
        // [directory]
        *p++ = '[';
        while (*name && name_len < 36) {
            *p++ = *name++;
            name_len++;
        }
        *p++ = ']';
        *p = 0;
        put_multi_char_xy(3 + name_len, screen_line, ' ', 36 - name_len);
    } else {
        uint32_t size = entry->size;
        if (size >= 10000) {
            uint32_t kb = (size + 512) / 1024;
            u32toa(kb, size_str);
            size_field_len = strlen(size_str) + 1;  // +1 for 'K'
        } else {
            u32toa(size, size_str);
            size_field_len = strlen(size_str);
        }

        // Limit filename length to (38 - size_field_len)
        uint8_t max_name_len = 38 - size_field_len;
        while (*name && name_len < max_name_len) {
            *p++ = *name++;
            name_len++;
        }

        // Add padding spaces (optional: skip if unnecessary)
        while ((uint8_t)(p - fileinfo) < (38 - size_field_len)) {
            *p++ = ' ';
        }

        // Add size
        char *s = size_str;
        while (*s) *p++ = *s++;
        if (size >= 10000)
            *p++ = 'k';

        *p = 0;
    }

    put_str_xy(1, screen_line, fileinfo);
}

void display_items(uint16_t offset) {
  int16_t items_to_display=dir_items;
  uint8_t i;
  uint8_t j;
  char line[40];
  if (dir_items - offset > VISIBLE_ENTRIES)
    items_to_display = VISIBLE_ENTRIES;
  else
    items_to_display = dir_items - offset;
  for(i=0; i<items_to_display; i++) {
    display_item(offset + i, i+2);
  }
  memset(line, ' ', 38);
  line[38] = 0;
  for(i=items_to_display; i<VISIBLE_ENTRIES; i++)
    put_str_xy(1, i+2, line);
}

void select_file(uint8_t index) {
    uint16_t old_line = file_selected - file_offset + 2;
    uint16_t new_line;

    // Clear old selection indicators
    put_char_attr_xy(0, old_line, ' ', 0x75);
    put_char_attr_xy(39, old_line, ' ', 0x75);

    uint8_t needs_redraw = 0;

    // Determine if full redraw is needed based on new index
    if (index > file_selected + 1) {
        if (index > file_offset + VISIBLE_ENTRIES - 2) {
            file_offset = (index == dir_items - 1)
                          ? index - VISIBLE_ENTRIES + 1
                          : index - VISIBLE_ENTRIES + 2;
            needs_redraw = 1;
        }
    } else if (index + 1 < file_selected) {
        if (index < file_offset + 1) {
            file_offset = (index == 0) ? 0 : index - 1;
            needs_redraw = 1;
        }
    } else if ((index > file_offset + VISIBLE_ENTRIES - 2) && (index < dir_items - 1)) {
        file_offset = index - VISIBLE_ENTRIES + 2;
        scroll_up();
        display_item(file_offset + VISIBLE_ENTRIES - 1, 22);
    } else if ((index < file_offset + 1) && (index > 0)) {
        file_offset = index - 1;
        scroll_down();
        display_item(file_offset, 2);
    }

    if (needs_redraw) {
        put_multi_attr_xy(1, old_line, 0x71, 38);
        display_items(file_offset);
    } else {
        put_multi_attr_xy(1, old_line, 0x71, 38);
    }

    // Draw new selection indicators
    new_line = index - file_offset + 2;
    put_multi_attr_xy(1, new_line, 0x16, 38);
    put_char_attr_xy(0, new_line, 0xFC, 0xa5);
    put_char_attr_xy(39, new_line, 0xFA, 0xa5);

    file_selected = index;

    // Display file selection status
    char buff[10];
    sprintf(buff, "[%3d/%3d]", file_selected + 1, dir_items);
    put_str_xy(1, 23, buff);
}


void select_next(uint8_t delta) {
  if (dir_items == 0)
    return;
  if (file_selected + delta >= dir_items)
    delta = dir_items - file_selected - 1;
  if (file_selected < dir_items-1)
    select_file(file_selected+delta);
}

void select_prev(uint8_t delta) {
  if (dir_items == 0)
    return;
  if (file_selected < delta)
    delta = file_selected;
  if (file_selected != 0)
    select_file(file_selected-delta);
}

void display_path(char *path) {
  put_str_xy(17, 1, path);
  uint16_t ln = strlen(path);
  put_multi_char_xy(17+ln, 1, ' ', 21-ln);
}

void remove_last_dir(char *path) {
  size_t len = strlen(path);

  // Remove trailing slash if present (but not if it's the root "/")
  if (len > 1 && path[len - 1] == '/') {
    path[len - 1] = '\0';
    len--;
  }

  // Find the last slash
  char *last_slash = strrchr(path, '/');
  if (last_slash && last_slash != path) {
    *last_slash = '\0';  // Truncate at last slash
  } else {
    // Keep root slash, or set to empty if nothing left
    if (last_slash == path)
      path[1] = '\0';
    else
      path[0] = '\0';
  }
}

void execute_selection(void) {
  if (dir_items == 0)
    return;
  if (entries[file_selected].isDir) {
    if (strcmp(entries[file_selected].filename, "..") == 0) {
      remove_last_dir(path);   
    } else {
      if (path[strlen(path) - 1] != '/') {
        strcat(path, "/");
      }
      strncat(path, entries[file_selected].filename, sizeof(path) - strlen(path) - 1);
    };
    display_path(path);
    read_dir(path);
    display_items(0);
    select_file(0); 
  } else {
    border(0);
    clrscr();
    put_str_xy(7, 10, "LOADING");
    put_str_xy(15, 10, entries[file_selected].filename);
#ifndef MZ800PICO_TEST
    set_command_data(file_selected & 0x00ff); 
    set_command_data(file_selected >> 8); 
    exec_command(0x03);
    while (get_command_status() != 0x03);
#endif
    relocate_and_execute();
  };
}

main() {
  uint16_t i;
  unsigned char val;
  char c;

  dir_items = 0;
  file_selected = 0;
  file_offset = 0;
  strcpy(path, "/");

  clrscr();
  border(0x09);
  put_str_attr_xy(17, 0,  "MZPico", 0x70);
  put_char_attr_xy(16, 0, 0xfe, 0x01);
  put_char_attr_xy(23, 0, 0xfd, 0x01);
  put_str_attr_xy(0, 24, "    Nav    Srch   Exe   Inf   Set   Quit", 0x70);
  put_str_attr_xy(0, 24, "\xc1\xc2\xc3\xc4", 0x60);
  put_str_attr_xy(8, 24, "A-Z", 0x06);
  put_str_attr_xy(16, 24, "CR", 0x06);
  put_str_attr_xy(22, 24, "F1", 0x06);
  put_str_attr_xy(28, 24, "F2", 0x06);
  put_str_attr_xy(34, 24, "F5", 0x06);

  put_str_attr_xy(0, 1,  " Device: [Flash]                        ", 0x05);
  for (i=2; i<24; i++) {
    put_char_attr_xy(0, i, ' ', 0x05);
    put_char_attr_xy(39, i, ' ', 0x05);
  }
  for (i=0; i<40; i++) {
    put_char_attr_xy(i, 23, ' ', 0x05);
  }
  put_char_attr_xy(0, 1, 0xfe, 0x51);
  put_char_attr_xy(39, 1, 0xfd, 0x51);
  put_char_attr_xy(0, 23, 0xfd, 0x15);
  put_char_attr_xy(39, 23, 0xfe, 0x15);
  display_path(path);
  read_dir(path);
  display_items(0);
  if (dir_items>0)
    select_file(0);
  else
    put_str_xy(5, 10, "No files found on this device");

  while (1) {
    if (c=inkey()) {
      switch (c) {
        case 0x11:
          select_next(1);
          break;
        case 0x12:
          select_prev(1);
          break;
        case 0x13:
          select_next(20);
          break;
        case 0x14:
          select_prev(20);
          break;
        case 0x0a:
          execute_selection();
          break;
        default:
          printf("Char: %02x\n", c);
      }
    }
  }
}
