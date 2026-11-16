#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "flash_sim.h"
typedef unsigned int uint;
static inline uint get_core_num(void) { return 1; }
static inline void tight_loop_contents(void) {}
static inline void __dmb(void) { __sync_synchronize(); }
static inline uint32_t time_us_32(void) { return 0; }
