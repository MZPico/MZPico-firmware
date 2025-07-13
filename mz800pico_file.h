#ifndef __MZ800PICO_FILE_H__
#define __MZ800PICO_FILE_H__

#include <stdint.h>
#include "mz800pico_gen_rd.h"

#define MAX_DIR_FILES 930

int read_directory(GEN_RD *dest_rd, char *path);
int mount_file(GEN_RD *dir_rd, GEN_RD *mount_rd, uint16_t index);

#endif
