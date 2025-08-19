cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)

project(mzpico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

set(PICO_BOARD_HEADER_DIRS ${CMAKE_SOURCE_DIR})
set(PICO_BOARD board)

# Initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable (binary name is now "mz800pico")
add_executable(mzpico)

# 16megs of flash on purple pico clones
target_compile_definitions(mzpico PRIVATE
    PICO_FLASH_SIZE_BYTES=16777216
)

# Generate PIO header from PIO source
pico_generate_pio_header(mzpico ${CMAKE_CURRENT_LIST_DIR}/trap_read.pio)
pico_generate_pio_header(mzpico ${CMAKE_CURRENT_LIST_DIR}/trap_write.pio)
pico_generate_pio_header(mzpico ${CMAKE_CURRENT_LIST_DIR}/trap_reset.pio)

target_sources(mzpico PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/main.c
    ${CMAKE_CURRENT_LIST_DIR}/device.c
    ${CMAKE_CURRENT_LIST_DIR}/file.c
    ${CMAKE_CURRENT_LIST_DIR}/fdc.c
    ${CMAKE_CURRENT_LIST_DIR}/qd.c
    ${CMAKE_CURRENT_LIST_DIR}/trap_read.pio
    ${CMAKE_CURRENT_LIST_DIR}/trap_write.pio
    ${CMAKE_CURRENT_LIST_DIR}/trap_reset.pio
    ${CMAKE_CURRENT_LIST_DIR}/msc_disk.c
    ${CMAKE_CURRENT_LIST_DIR}/usb_descriptors.c
    ${CMAKE_CURRENT_LIST_DIR}/fatfs_disk.c
    ${CMAKE_CURRENT_LIST_DIR}/flash_fs.c
    ${CMAKE_CURRENT_LIST_DIR}/fatfs/ff.c
    ${CMAKE_CURRENT_LIST_DIR}/fatfs/ffunicode.c
    ${CMAKE_CURRENT_LIST_DIR}/fatfs/diskio.c
)

target_include_directories(mzpico PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/fatfs
)

# Link with required libraries
target_link_libraries(mzpico
    pico_stdlib
    hardware_pio
    hardware_irq
    hardware_flash
    tinyusb_device
)

# Enable USB stdio
pico_enable_stdio_usb(mzpico 1)
pico_enable_stdio_uart(mzpico 0)

# Create UF2, bin, and hex files
pico_add_extra_outputs(mzpico)

