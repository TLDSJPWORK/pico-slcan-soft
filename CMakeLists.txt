# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set minimum required version of CMake
cmake_minimum_required(VERSION 3.13)

# Defaults to the Pico board (a value of 'pico'). Uncomment the following line if using an Pico W.
# set(PICO_BOARD pico_w)
set(PICO_BOARD pico CACHE STRING "Board type")
option(LISTEN_ONLY "Do not transmit CAN frames" OFF)

# Include build functions from Pico SDK
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

# NVM領域サイズ（4KB）
set(NVM_SIZE 4096)

# フラッシュ全体サイズ（RP2040は通常2MB）
set(FLASH_TOTAL_SIZE 2097152)

# アプリが使えるサイズ（末尾4KBを除外）
math(EXPR PICO_FLASH_SIZE_BYTES "${FLASH_TOTAL_SIZE} - ${NVM_SIZE}")
set(PICO_FLASH_SIZE_BYTES_STRING "${PICO_FLASH_SIZE_BYTES}")

message(STATUS "App flash size: ${PICO_FLASH_SIZE_BYTES_STRING} bytes")

# Set name of project (as PROJECT_NAME) and C/C++ standards
project(pico-slcan C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# These are the definitions to select the MCP25xxFD on the CANPico and
# the binding to the RP2040 and CANPico pinouts.
# add_definitions(-DMCP25xxFD -DHOST_CANPICO)
add_definitions(-DMCP25xxFD -DHOST_PICO_SLCAN -DPICO_FLASH_SIZE_BYTES_STRING=${PICO_FLASH_SIZE_BYTES_STRING})
# add_definitions(-DMCP25xxFD -DHOST_PICO_SLCAN)
# Prevent hello world from transmitting CAN frames
if(LISTEN_ONLY)
add_definitions(-DLISTEN_ONLY)
endif()

# Creates a pico-sdk sußbdirectory in our project for the libraries
pico_sdk_init()

# Tell CMake where to find the executable source file
add_executable(${PROJECT_NAME} 
    pico-slcan.c
    slcan/slcan.c
    mcp25xxfd/mcp25xxfd.c
    pico-ssd1306/ssd1306.c
)

# Create map/bin/hex/uf2 files
pico_add_extra_outputs(${PROJECT_NAME})

# Link to pico_stdlib (gpio, time, etc. functions)
target_link_libraries(${PROJECT_NAME} 
    pico_stdlib
    pico_multicore
    hardware_spi
    hardware_gpio
    hardware_sync
    hardware_i2c
)

# Additional library for Pico W to access the LED via the WiFi chip
# (See https://github.com/raspberrypi/pico-sdk/releases for a list of other libraries)
if (${PICO_BOARD} MATCHES "pico_w") 
    target_link_libraries(${PROJECT_NAME} 
        pico_cyw43_arch_none
)
endif()


# For standalone execution, set USB to 1 and UART to 0 so that the output can be
# displayed over the USB serial port. For use with a picoprobe SWD debugger, use
# UART to 1 and USB to 0: the text will appear over USB serial but from the picoprobe
# pico.
#
# See the blog post:
#
# https://kentindell.github.io/2022/07/26/canpico-c-debug/
#
# for how to set up a picoprobe debugging system for C development on the Pico.
#
pico_enable_stdio_usb(${PROJECT_NAME} 1)
pico_enable_stdio_uart(${PROJECT_NAME} 0)
