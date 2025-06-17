cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)

project(mz800pico_project C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable (binary name is now "mz800pico")
add_executable(mz800pico
    main.c
    mz800pico_trap.pio
)

# Generate PIO header from PIO source
pico_generate_pio_header(mz800pico ${CMAKE_CURRENT_LIST_DIR}/mz800pico_trap.pio)

# Link with required libraries
target_link_libraries(mz800pico
    pico_stdlib
    hardware_pio
    hardware_irq
)

# Enable USB stdio
pico_enable_stdio_usb(mz800pico 1)
pico_enable_stdio_uart(mz800pico 0)

# Create UF2, bin, and hex files
pico_add_extra_outputs(mz800pico)

