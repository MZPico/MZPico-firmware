#!/bin/sh
# Host tests of the COW RAM disk ([ramdisk] mode=cow): src/mz_devices/cow_store.*
# (the bus-path logic) and cow_resolve.* (file -> XIP page table) against the
# real FatFS on an emulated log-structured flash_fs. See test.cpp.
set -e
cd "$(dirname "$0")"
R=../../src
FF=../../external/fatfs-sdk/src/ff15/source
OUT=${TMPDIR:-/tmp}/cow_ramdisk_test
cc -O1 -w -I stubs -I $R -I $FF -c $FF/ff.c -o $OUT.ff.o
cc -O1 -w -I stubs -I $R -I $FF -c $FF/ffunicode.c -o $OUT.ffu.o
cc -O1 -w -I stubs -I $R -I $FF -c $FF/ffsystem.c -o $OUT.ffs.o
g++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
    -DCOW_HOST_ATOMIC_BUSY -I stubs -I $R -I $R/mz_devices -I $FF \
    test.cpp $R/mz_devices/cow_store.cpp $R/mz_devices/cow_resolve.cpp \
    $OUT.ff.o $OUT.ffu.o $OUT.ffs.o -lpthread -fsanitize=address,undefined -o $OUT
$OUT
