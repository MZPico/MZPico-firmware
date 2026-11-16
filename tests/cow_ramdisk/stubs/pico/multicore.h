#pragma once
#include <stdbool.h>
static inline bool multicore_lockout_victim_is_initialized(unsigned core) { (void)core; return false; }
static inline void multicore_lockout_start_blocking(void) {}
static inline void multicore_lockout_end_blocking(void) {}
