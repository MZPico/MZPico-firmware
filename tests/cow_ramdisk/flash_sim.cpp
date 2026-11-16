// See stubs/flash_sim.h
#include "flash_sim.h"
#include <sys/mman.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

uint64_t sim_flash_us = 0;
uint32_t sim_erases = 0, sim_programs = 0, sim_overprograms = 0, sim_ops = 0;
void (*sim_op_hook)(int kind, uint32_t off, size_t n) = nullptr;

// The chip, mapped where the RP2040 maps it, before main()
static struct SimMap {
    SimMap() {
        void* p = mmap((void*)(uintptr_t)XIP_BASE, PICO_FLASH_SIZE_BYTES, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != (void*)(uintptr_t)XIP_BASE) {
            fprintf(stderr, "flash_sim: cannot map 0x%08x\n", XIP_BASE);
            exit(2);
        }
        memset(p, 0xFF, PICO_FLASH_SIZE_BYTES);
    }
} g_sim_map;

static uint8_t* chip(uint32_t off) { return (uint8_t*)(uintptr_t)(XIP_BASE + off); }

extern "C" void flash_range_erase(uint32_t off, size_t n) {
    if (off % FLASH_SECTOR_SIZE || n % FLASH_SECTOR_SIZE || off + n > PICO_FLASH_SIZE_BYTES) abort();
    if (sim_op_hook) sim_op_hook(0, off, n);
    sim_ops++;
    memset(chip(off), 0xFF, n);
    sim_erases += (uint32_t)(n / FLASH_SECTOR_SIZE);
    sim_flash_us += (uint64_t)SIM_ERASE_US * (n / FLASH_SECTOR_SIZE);
}

extern "C" void flash_range_program(uint32_t off, const uint8_t* data, size_t n) {
    if (off % FLASH_PAGE_SIZE || n % FLASH_PAGE_SIZE || off + n > PICO_FLASH_SIZE_BYTES) abort();
    if (sim_op_hook) sim_op_hook(1, off, n);
    sim_ops++;
    uint8_t* p = chip(off);
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0xFF) sim_overprograms++;   // programming a non-erased byte
        p[i] &= data[i];
    }
    sim_programs += (uint32_t)(n / FLASH_PAGE_SIZE);
    sim_flash_us += (uint64_t)SIM_PROGRAM_US * (n / FLASH_PAGE_SIZE);
}
