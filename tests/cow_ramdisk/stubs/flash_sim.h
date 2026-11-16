#pragma once
// Host stand-in for the RP2040 flash chip under the real src/flash_fs.c
// (tests/cow_ramdisk). The 2 MB chip is mapped at its XIP address
// (0x10000000) so flash_fs's 32-bit address arithmetic works unchanged;
// erase/program follow NOR rules (program only clears bits) and advance a
// simulated clock by the W25Q16JV's typical times.
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define XIP_BASE 0x10000000u
#define PICO_FLASH_SIZE_BYTES (2u * 1024u * 1024u)
#define FLASH_SECTOR_SIZE 4096u
#define FLASH_PAGE_SIZE 256u
#define SIM_ERASE_US 45000u          // 4 KB sector erase, typical
#define SIM_PROGRAM_US 400u          // 256 B page program, typical

void flash_range_erase(uint32_t off, size_t n);
void flash_range_program(uint32_t off, const uint8_t* data, size_t n);
extern uint64_t sim_flash_us;        // time spent in flash operations
extern uint32_t sim_erases, sim_programs, sim_overprograms;
// Power-cut capture: before flash op number sim_cut_at (counted from
// sim_ops), the hook gets the op (0 erase, 1 program) and its range
extern uint32_t sim_ops;
extern void (*sim_op_hook)(int kind, uint32_t off, size_t n);
#ifdef __cplusplus
}
#endif
