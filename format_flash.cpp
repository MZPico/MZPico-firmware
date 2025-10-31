#include "pico/stdlib.h"
#include "fatfs_disk.h"

int main(void) {
    create_fatfs_disk();
    while (true) tight_loop_contents();
}

