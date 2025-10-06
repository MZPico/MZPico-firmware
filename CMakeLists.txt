cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)

project(mzpico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)


set(PICO_BOARD_HEADER_DIRS ${CMAKE_SOURCE_DIR})
set(PICO_BOARD board)

# Initialize the Raspberry Pi Pico SDK
pico_sdk_init()

add_subdirectory(fatfs-sdk/src build)
add_subdirectory(byte_source)
add_executable(mzpico)

# 16megs of flash on purple pico clones
target_compile_definitions(mzpico PRIVATE
        #PICO_FLASH_SIZE_BYTES=16777216
                PICO_FLASH_SIZE_BYTES=2097152
)

# Generate PIO header from PIO source
pico_generate_pio_header(mzpico ${CMAKE_CURRENT_LIST_DIR}/trap_reset.pio)

target_sources(mzpico PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/main.cpp
    ${CMAKE_CURRENT_LIST_DIR}/device.cpp
    ${CMAKE_CURRENT_LIST_DIR}/file.cpp
    ${CMAKE_CURRENT_LIST_DIR}/config.cpp
    ${CMAKE_CURRENT_LIST_DIR}/mz_devices.cpp
    ${CMAKE_CURRENT_LIST_DIR}/mz_devices/qd.cpp
    ${CMAKE_CURRENT_LIST_DIR}/mz_devices/pico_mgr.cpp
    ${CMAKE_CURRENT_LIST_DIR}/mz_devices/fdc.cpp
    ${CMAKE_CURRENT_LIST_DIR}/mz_devices/sramdisk.cpp
    ${CMAKE_CURRENT_LIST_DIR}/mz_devices/pico_rd.cpp
    ${CMAKE_CURRENT_LIST_DIR}/iniparser/src/iniparser.c
    ${CMAKE_CURRENT_LIST_DIR}/iniparser/src/dictionary.c
    ${CMAKE_CURRENT_LIST_DIR}/trap_reset.pio
    ${CMAKE_CURRENT_LIST_DIR}/msc_disk.c
    ${CMAKE_CURRENT_LIST_DIR}/usb_descriptors.c
    ${CMAKE_CURRENT_LIST_DIR}/hw_config.c
    ${CMAKE_CURRENT_LIST_DIR}/fatfs_disk.c
    ${CMAKE_CURRENT_LIST_DIR}/flash_fs.c
    ${CMAKE_CURRENT_LIST_DIR}/fatfs_glue.c
    ${BYTE_SOURCE_SOURCES}
)

target_include_directories(mzpico PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/byte_source
    ${CMAKE_CURRENT_LIST_DIR}/mz_devices
    ${CMAKE_CURRENT_LIST_DIR}/iniparser/src
    ${CMAKE_CURRENT_LIST_DIR}/fatfs-sdk/src/ff15/source/
)

# Link with required libraries
target_link_libraries(mzpico
    pico_stdlib
    hardware_pio
    hardware_irq
    hardware_flash
    tinyusb_device
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)

set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/fatfs-sdk/src/src/glue.c
    PROPERTIES HEADER_FILE_ONLY TRUE
)

# Enable USB stdio
pico_enable_stdio_usb(mzpico 1)
pico_enable_stdio_uart(mzpico 0)

# Create UF2, bin, and hex files
pico_add_extra_outputs(mzpico)

