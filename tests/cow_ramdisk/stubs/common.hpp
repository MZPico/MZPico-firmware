#pragma once
// Host stand-in for src/common.hpp: no RAM section on the host (a section
// named .data.* would land the code in a non-executable segment)
#include <stdint.h>
#define ALWAYS_INLINE inline __attribute__((always_inline))
#define RAM_FUNC
