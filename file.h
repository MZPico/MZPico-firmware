#ifndef __FILE_H__
#define __FILE_H__

#include <stdint.h>
#include "gen_rd.h"

#define MAX_DIR_FILES 930

int read_directory(char *path, GEN_RD *buffer);
int mount_file(char *path, GEN_RD *buffer);

#endif
