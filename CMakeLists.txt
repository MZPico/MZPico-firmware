cmake_minimum_required(VERSION 3.13)
set(PICO_BOARD_HEADER_DIRS ${CMAKE_SOURCE_DIR})
set(PICO_BOARD board)
set(PICO_SDK_PATH "${CMAKE_SOURCE_DIR}/pico-sdk")
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(mzpico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(PICO_BOARD_HEADER_DIRS ${CMAKE_SOURCE_DIR})
set(PICO_BOARD board)
set(GEN_MZF_SCRIPT ${CMAKE_SOURCE_DIR}/gen_mzf_array.py)
set(BOARD "FRUGAL" CACHE STRING "Select the board type (FRUGAL, DELUXE)")
set_property(CACHE BOARD PROPERTY STRINGS FRUGAL DELUXE)
set(FLASH_SIZE "2M" CACHE STRING "Flash size (use 2M or 16M)")
set_property(CACHE FLASH_SIZE PROPERTY STRINGS 2M 16M)

if(FLASH_SIZE STREQUAL "2M")
    set(FLASH_SIZE_BYTES 2097152)
elseif(FLASH_SIZE STREQUAL "16M")
    set(FLASH_SIZE_BYTES 16777216)
else()
    message(FATAL_ERROR "Invalid FLASH_SIZE value. Use one of: 2M, 16M")
endif()

message(STATUS "Board type: ${BOARD}")
message(STATUS "Flash size selected: ${FLASH_SIZE}")

add_subdirectory(manager)

set(GENERATED_MZF_DIR ${CMAKE_SOURCE_DIR})

set(GENERATED_HEADERS "")
foreach(MZF_FILE ${MZF_FILES})
    get_filename_component(MZF_NAME ${MZF_FILE} NAME_WE)   # e.g. explorer.mzf → explorer
    set(HEADER_FILE ${GENERATED_MZF_DIR}/mzf_${MZF_NAME}.hpp)
    list(APPEND GENERATED_HEADERS ${HEADER_FILE})

    add_custom_command(
        OUTPUT ${HEADER_FILE}
        COMMAND ${CMAKE_COMMAND} -E echo "Generating header from ${MZF_FILE}"
        COMMAND ${GEN_MZF_SCRIPT}
                ${MZF_FILE} ${HEADER_FILE}
        DEPENDS ${MZF_FILE}
        VERBATIM
    )
endforeach()

add_custom_target(generate_mzf_headers ALL DEPENDS ${GENERATED_HEADERS})
add_dependencies(generate_mzf_headers build_explorer build_menu)

# Initialize the Raspberry Pi Pico SDK
pico_sdk_init()

add_subdirectory(fatfs-sdk/src build)
add_subdirectory(byte_source)
add_executable(mzpico)

add_dependencies(mzpico generate_mzf_headers)

string(TOLOWER ${BOARD} BOARD_L)
string(TOLOWER ${FLASH_SIZE} FLASH_L)

set_target_properties(mzpico PROPERTIES
    OUTPUT_NAME "mzpico_${BOARD_L}_${FLASH_L}"
)

target_compile_definitions(mzpico PRIVATE BOARD_${BOARD})

target_compile_definitions(mzpico PRIVATE
    PICO_FLASH_SIZE_BYTES=${FLASH_SIZE_BYTES}
)

# Generate PIO header from PIO source
pico_generate_pio_header(mzpico ${CMAKE_CURRENT_LIST_DIR}/bus_io.pio)

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
    ${CMAKE_CURRENT_LIST_DIR}/bus_io.pio
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

add_executable(mzpico_format)

set_target_properties(mzpico_format PROPERTIES
    OUTPUT_NAME "mzpico_format_${FLASH_L}"
)

target_sources(mzpico_format PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/format_flash.cpp
    ${CMAKE_CURRENT_LIST_DIR}/msc_disk.c
    ${CMAKE_CURRENT_LIST_DIR}/usb_descriptors.c
    ${CMAKE_CURRENT_LIST_DIR}/hw_config.c
    ${CMAKE_CURRENT_LIST_DIR}/fatfs_disk.c
    ${CMAKE_CURRENT_LIST_DIR}/flash_fs.c
    ${CMAKE_CURRENT_LIST_DIR}/fatfs_glue.c
)

target_include_directories(mzpico_format PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/fatfs-sdk/src/ff15/source/
)


# inherit same compile definitions as mzpico
target_compile_definitions(mzpico_format PRIVATE
    PICO_FLASH_SIZE_BYTES=${FLASH_SIZE_BYTES}
)

target_link_libraries(mzpico_format
    pico_stdlib
    hardware_flash
    tinyusb_device
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)

# You need this so you get the UF2/bin/hex files
pico_add_extra_outputs(mzpico_format)