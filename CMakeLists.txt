cmake_minimum_required(VERSION 3.13)

set(BOARD "FRUGAL" CACHE STRING "Select the board type (FRUGAL, DELUXE)")
set_property(CACHE BOARD PROPERTY STRINGS FRUGAL DELUXE)
set(FLASH_SIZE "2M" CACHE STRING "Flash size (use 2M or 16M)")
set_property(CACHE FLASH_SIZE PROPERTY STRINGS 2M 16M)
option(USE_PICO_W "Enable Pico W WiFi cloud file support" OFF)


set(SRC_ROOT ${CMAKE_SOURCE_DIR}/src)
set(EXTERNAL_ROOT ${CMAKE_SOURCE_DIR}/external)
set(SCRIPTS_ROOT ${CMAKE_SOURCE_DIR}/scripts)

set(PICO_BOARD_HEADER_DIRS ${SRC_ROOT})
if(USE_PICO_W)
    set(PICO_BOARD pico_w)
else()
    set(PICO_BOARD board)
endif()
set(PICO_SDK_PATH "${EXTERNAL_ROOT}/pico-sdk")
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(mzpico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(PICO_BOARD_HEADER_DIRS ${CMAKE_SOURCE_DIR})
set(GEN_MZF_SCRIPT ${SCRIPTS_ROOT}/gen_mzf_array.py)

if(FLASH_SIZE STREQUAL "2M")
    set(FLASH_SIZE_BYTES 2097152)
elseif(FLASH_SIZE STREQUAL "16M")
    set(FLASH_SIZE_BYTES 16777216)
else()
    message(FATAL_ERROR "Invalid FLASH_SIZE value. Use one of: 2M, 16M")
endif()

message(STATUS "Board type: ${BOARD}")
message(STATUS "Flash size selected: ${FLASH_SIZE}")
message(STATUS "Pico W WiFi support: ${USE_PICO_W}")

add_subdirectory(${EXTERNAL_ROOT}/manager)

set(GENERATED_MZF_DIR ${SRC_ROOT})

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

add_subdirectory(${EXTERNAL_ROOT}/fatfs-sdk/src build)
add_subdirectory(${SRC_ROOT}/byte_source)
add_executable(mzpico)

# ---- Version tagging (passed via -DVERSION=tag) ----
if(DEFINED VERSION)
    set(VERSION_TAG "${VERSION}")
    message(STATUS "Firmware version: ${VERSION_TAG}")
else()
    set(VERSION_TAG "")
    message(STATUS "Firmware version: (none)")
endif()

add_dependencies(mzpico generate_mzf_headers)

string(TOLOWER ${BOARD} BOARD_L)
string(TOLOWER ${FLASH_SIZE} FLASH_L)

set(_MZPICO_OUT "mzpico_${BOARD_L}_${FLASH_L}")
if(USE_PICO_W)
    set(_MZPICO_OUT "${_MZPICO_OUT}_w")
endif()
if(VERSION_TAG)
    set(_MZPICO_OUT "${_MZPICO_OUT}_${VERSION_TAG}")
endif()
set_target_properties(mzpico PROPERTIES OUTPUT_NAME "${_MZPICO_OUT}")

target_compile_definitions(mzpico PRIVATE BOARD_${BOARD})
if(DEFINED VERSION)
    target_compile_definitions(mzpico PRIVATE FW_VERSION=\"${VERSION}\")
endif()

target_compile_definitions(mzpico PRIVATE
    PICO_FLASH_SIZE_BYTES=${FLASH_SIZE_BYTES}
)

# Generate PIO header from PIO source
pico_generate_pio_header(mzpico ${SRC_ROOT}/bus_io.pio)
pico_generate_pio_header(mzpico ${SRC_ROOT}/i2s_audio.pio)

target_sources(mzpico PUBLIC
    ${SRC_ROOT}/main.cpp
    ${SRC_ROOT}/device.cpp
    ${SRC_ROOT}/file.cpp
    ${SRC_ROOT}/cloud_fs.cpp
    ${SRC_ROOT}/http_utils.c
    ${SRC_ROOT}/config.cpp
    ${SRC_ROOT}/mz_devices.cpp
    ${SRC_ROOT}/mz_devices/qd.cpp
    ${SRC_ROOT}/mz_devices/pico_mgr.cpp
    ${SRC_ROOT}/mz_devices/fdc.cpp
    ${SRC_ROOT}/mz_devices/sramdisk.cpp
    ${SRC_ROOT}/mz_devices/pico_rd.cpp
    ${SRC_ROOT}/mz_devices/sn76489.cpp
    ${EXTERNAL_ROOT}/iniparser/src/iniparser.c
    ${EXTERNAL_ROOT}/iniparser/src/dictionary.c
    ${SRC_ROOT}/bus_io.pio
    ${SRC_ROOT}/i2s_audio.pio
    ${SRC_ROOT}/msc_disk.c
    ${SRC_ROOT}/usb_descriptors.c
    ${SRC_ROOT}/hw_config.c
    ${SRC_ROOT}/fatfs_disk.c
    ${SRC_ROOT}/flash_fs.c
    ${SRC_ROOT}/fatfs_glue.c
    ${BYTE_SOURCE_SOURCES}
)

target_include_directories(mzpico PUBLIC
    ${SRC_ROOT}
    ${SRC_ROOT}/byte_source
    ${SRC_ROOT}/mz_devices
    ${SRC_ROOT}
    ${EXTERNAL_ROOT}/iniparser/src
    ${EXTERNAL_ROOT}/fatfs-sdk/src/ff15/source/
)

# Link with required libraries
target_link_libraries(mzpico
    pico_stdlib
    hardware_pio
    hardware_irq
    hardware_flash
    pico_multicore
    tinyusb_device
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)


if(USE_PICO_W)
    # Polling architecture (we will poll on core1)
    target_link_libraries(mzpico pico_cyw43_arch_lwip_poll pico_lwip_http)
    target_compile_definitions(mzpico PRIVATE USE_PICO_W PICO_CYW43_ARCH_POLL)
endif()

set_source_files_properties(
    ${EXTERNAL_ROOT}/fatfs-sdk/src/src/glue.c
    PROPERTIES HEADER_FILE_ONLY TRUE
)

# Enable USB stdio
pico_enable_stdio_usb(mzpico 1)
pico_enable_stdio_uart(mzpico 0)

# Create UF2, bin, and hex files
pico_add_extra_outputs(mzpico)

add_executable(mzpico_format)

set(_MZPICO_FORMAT_OUT "mzpico_format_${FLASH_L}")
if(VERSION_TAG)
    set(_MZPICO_FORMAT_OUT "${_MZPICO_FORMAT_OUT}_${VERSION_TAG}")
endif()
set_target_properties(mzpico_format PROPERTIES OUTPUT_NAME "${_MZPICO_FORMAT_OUT}")

target_sources(mzpico_format PUBLIC
    ${SRC_ROOT}/format_flash.cpp
    ${SRC_ROOT}/msc_disk.c
    ${SRC_ROOT}/usb_descriptors.c
    ${SRC_ROOT}/hw_config.c
    ${SRC_ROOT}/fatfs_disk.c
    ${SRC_ROOT}/flash_fs.c
    ${SRC_ROOT}/fatfs_glue.c
)

target_include_directories(mzpico_format PUBLIC
    ${SRC_ROOT}
    ${EXTERNAL_ROOT}/fatfs-sdk/src/ff15/source/
)


# inherit same compile definitions as mzpico
target_compile_definitions(mzpico_format PRIVATE
    PICO_FLASH_SIZE_BYTES=${FLASH_SIZE_BYTES}
)
if(DEFINED VERSION)
    target_compile_definitions(mzpico_format PRIVATE FW_VERSION=\"${VERSION}\")
endif()

target_link_libraries(mzpico_format
    pico_stdlib
    hardware_flash
    pico_multicore
    tinyusb_device
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)

# You need this so you get the UF2/bin/hex files
pico_add_extra_outputs(mzpico_format)
