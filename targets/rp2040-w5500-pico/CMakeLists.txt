cmake_minimum_required(VERSION 3.13)

# Standard Pico SDK bootstrap -- pico_sdk_import.cmake is copied verbatim
# from $PICO_SDK_PATH/external/pico_sdk_import.cmake into THIS directory
# (see README "Setup").
include(pico_sdk_import.cmake)

project(katcp_w5500_pico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Code shared with other targets (e.g. targets/stm32-e407) lives one
# level up, outside any single target's own directory.
set(COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../common)

add_executable(katcp_w5500_pico
    src/main.c
    src/platform_pico.c
    ${COMMON_DIR}/src/katcp_proto.c
    ${COMMON_DIR}/src/katcp_commands.c
    ${COMMON_DIR}/src/synth_stub.c
    ${COMMON_DIR}/src/sensor.c
)

target_include_directories(katcp_w5500_pico PRIVATE
    ${COMMON_DIR}/include
)

# --- WIZnet ioLibrary_Driver -----------------------------------------------
# Vendored as a git submodule at third_party/ioLibrary_Driver (see README).
# RP2040/W5500-specific, so it's vendored under this target, not common/.
set(WIZNET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ioLibrary_Driver)

target_include_directories(katcp_w5500_pico PRIVATE
    ${WIZNET_DIR}/Ethernet
    ${WIZNET_DIR}/Ethernet/W5500
    ${WIZNET_DIR}/Internet
    ${WIZNET_DIR}/Internet/DHCP
)

file(GLOB WIZNET_SOURCES
    ${WIZNET_DIR}/Ethernet/*.c
    ${WIZNET_DIR}/Ethernet/W5500/*.c
    ${WIZNET_DIR}/Internet/DHCP/*.c
)
target_sources(katcp_w5500_pico PRIVATE ${WIZNET_SOURCES})

target_link_libraries(katcp_w5500_pico
    pico_stdlib
    hardware_spi
    hardware_gpio
)

# USB serial for printf() debugging; disable UART since it's not wired
# on the W5500-EVB-Pico by default.
pico_enable_stdio_usb(katcp_w5500_pico 1)
pico_enable_stdio_uart(katcp_w5500_pico 0)

pico_add_extra_outputs(katcp_w5500_pico)
