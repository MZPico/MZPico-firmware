#!/bin/sh
# Host tests of the COW RAM disk ([ramdisk] mode=cow): src/mz_devices/cow_store.*
# (the bus-path logic), cow_resolve.* (file -> XIP page table) and cow_image.*
# ([ramdisk] learn) against the real FatFS, on an emulated log-structured
# flash_fs (test.cpp) and on the real src/flash_fs.c over an emulated NOR
# chip (learn.cpp, flash_sim.cpp). See test.cpp and learn.cpp.
# Optional: a RAM-disk trace to replay (mz800pop rdlog), as the argument.
set -e
cd "$(dirname "$0")"
R=../../src
FF=../../external/fatfs-sdk/src/ff15/source
OUT=${TMPDIR:-/tmp}/cow_ramdisk_test
SAN="-fsanitize=address,undefined"
[ -n "$COW_NOSAN" ] && SAN=""
cc -O1 -w -I stubs -I $R -I $FF -c $FF/ff.c -o $OUT.ff.o
cc -O1 -w -I stubs -I $R -I $FF -c $FF/ffunicode.c -o $OUT.ffu.o
cc -O1 -w -I stubs -I $R -I $FF -c $FF/ffsystem.c -o $OUT.ffs.o
cc -O1 -g -w $SAN -I stubs -I $R -c $R/flash_fs.c -o $OUT.flash_fs.o
g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter $SAN \
    -DCOW_HOST_ATOMIC_BUSY -I stubs -I $R -I $R/mz_devices -I $FF \
    test.cpp learn.cpp flash_sim.cpp $R/mz_devices/cow_store.cpp $R/mz_devices/cow_resolve.cpp \
    $R/mz_devices/cow_image.cpp $OUT.flash_fs.o \
    $OUT.ff.o $OUT.ffu.o $OUT.ffs.o -lpthread $SAN -o $OUT
$OUT "$@"
