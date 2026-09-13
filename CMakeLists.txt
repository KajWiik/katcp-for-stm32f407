cmake_minimum_required(VERSION 3.13)

# Standard Pico SDK bootstrap -- pico_sdk_import.cmake is copied verbatim
# from $PICO_SDK_PATH/external/pico_sdk_import.cmake (see README "Setup").
include(pico_sdk_import.cmake)

project(katcp_w5500_pico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(katcp_w5500_pico
    src/main.c
    src/katcp_proto.c
    src/katcp_commands.c
    src/synth_stub.c
)

target_include_directories(katcp_w5500_pico PRIVATE
    include
)

# --- WIZnet ioLibrary_Driver -----------------------------------------------
# Vendored as a git submodule at third_party/ioLibrary_Driver (see README).
# We only need the Ethernet/ and Internet/ (socket API) pieces for W5500.
set(WIZNET_DIR ${CMAKE_SOURCE_DIR}/third_party/ioLibrary_Driver)

target_include_directories(katcp_w5500_pico PRIVATE
    ${WIZNET_DIR}/Ethernet
    ${WIZNET_DIR}/Ethernet/W5500
    ${WIZNET_DIR}/Internet
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
