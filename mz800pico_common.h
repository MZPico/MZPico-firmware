#ifndef __MZ800PICO_COMMON_H__
#define __MZ800PICO_COMMON_H__

#define ALWAYS_INLINE static inline __attribute__((always_inline))
#define RAM_FUNC __attribute__((section(".data.ram_func"))) __attribute__((noinline))

#endif
