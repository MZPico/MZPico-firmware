cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)

project(mz800pico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(PICO_BOARD_HEADER_DIRS ${CMAKE_SOURCE_DIR})
set(PICO_BOARD mz800pico_board)

# Initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable (binary name is now "mz800pico")
add_executable(mz800pico)

# 16megs of flash on purple pico clones
target_compile_definitions(mz800pico PRIVATE
    PICO_FLASH_SIZE_BYTES=16777216
)

# Generate PIO header from PIO source
pico_generate_pio_header(mz800pico ${CMAKE_CURRENT_LIST_DIR}/mz800pico_trap_read.pio)
pico_generate_pio_header(mz800pico ${CMAKE_CURRENT_LIST_DIR}/mz800pico_trap_write.pio)
pico_generate_pio_header(mz800pico ${CMAKE_CURRENT_LIST_DIR}/mz800pico_trap_reset.pio)

target_sources(mz800pico PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/main.c
    ${CMAKE_CURRENT_LIST_DIR}/mz800pico_device.c
    ${CMAKE_CURRENT_LIST_DIR}/mz800pico_file.c
    ${CMAKE_CURRENT_LIST_DIR}/mz800pico_trap_read.pio
    ${CMAKE_CURRENT_LIST_DIR}/mz800pico_trap_write.pio
    ${CMAKE_CURRENT_LIST_DIR}/mz800pico_trap_reset.pio
    ${CMAKE_CURRENT_LIST_DIR}/msc_disk.c
    ${CMAKE_CURRENT_LIST_DIR}/usb_descriptors.c
    ${CMAKE_CURRENT_LIST_DIR}/fatfs_disk.c
    ${CMAKE_CURRENT_LIST_DIR}/flash_fs.c
    ${CMAKE_CURRENT_LIST_DIR}/fatfs/ff.c
    ${CMAKE_CURRENT_LIST_DIR}/fatfs/ffunicode.c
    ${CMAKE_CURRENT_LIST_DIR}/fatfs/diskio.c
)

target_include_directories(mz800pico PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/fatfs
)

# Link with required libraries
target_link_libraries(mz800pico
    pico_stdlib
    hardware_pio
    hardware_irq
    hardware_flash
    tinyusb_device
)

# Enable USB stdio
pico_enable_stdio_usb(mz800pico 1)
pico_enable_stdio_uart(mz800pico 0)

# Create UF2, bin, and hex files
pico_add_extra_outputs(mz800pico)

