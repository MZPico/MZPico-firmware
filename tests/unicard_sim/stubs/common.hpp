#pragma once
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#define ALWAYS_INLINE inline __attribute__((always_inline))
#define RAM_FUNC
inline void blink(int) {}
inline void halt() { std::abort(); }
