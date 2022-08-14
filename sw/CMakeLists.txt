# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)
set(PICO_SDK_PATH "/Users/ian/src/pico/pico-sdk")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)
include(pico_extras_import.cmake)

project(sw-pico C CXX ASM)

# set(PICO_DEOPTIMIZED_DEBUG "1")
set(CMAKE_BUILD_TYPE "Release")
set(PICO_COPY_TO_RAM 1)
#set(SKIP_PICO_MALLOC 1)

set(PROJECT_TYPE "GUS")
# set(PROJECT_TYPE "OPL")

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1
add_executable(sw-pico sw-pico.cpp)

target_compile_definitions(sw-pico PRIVATE
        # compile time configuration of I2S
        #        PICO_AUDIO_I2S_MONO_INPUT=1
        USE_AUDIO_I2S=1
        PICO_AUDIO_I2S_DATA_PIN=16
        PICO_AUDIO_I2S_CLOCK_PIN_BASE=17
        PICO_DEFAULT_UART=1
        PICO_DEFAULT_UART_TX_PIN=20
        PICO_DEFAULT_UART_RX_PIN=21
        PICO_AUDIO_I2S_PIO=1
        # USE_IRQ=1
        PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=5000000
)

if(PROJECT_TYPE STREQUAL "GUS")
    target_sources(sw-pico PRIVATE gusplay.cpp pico_pic.cpp)
    target_compile_definitions(sw-pico PRIVATE
        SOUND_GUS=1
        PSRAM=1
        TEST_PSRAM=1
        USE_ALARM=1
        # USE_SPINLOCK=1
    )
    pico_generate_pio_header(sw-pico ${CMAKE_CURRENT_LIST_DIR}/psram_spi.pio)
endif()

if(PROJECT_TYPE STREQUAL "OPL")
    target_sources(sw-pico PRIVATE adlib.cpp)
    target_link_libraries(
        sw-pico
        opl
    )
    target_compile_definitions(sw-pico PRIVATE
        SOUND_OPL=1
        USE_EMU8950_OPL=1
    )
    add_subdirectory(opl)
endif()


pico_set_program_name(sw-pico "sw-pico")
pico_set_program_version(sw-pico "0.1")

pico_enable_stdio_uart(sw-pico 1)
pico_enable_stdio_usb(sw-pico 0)
pico_enable_stdio_semihosting(sw-pico 0)

pico_generate_pio_header(sw-pico ${CMAKE_CURRENT_LIST_DIR}/io.pio)

add_subdirectory(stdio_async_uart)

# Add the standard library to the build
target_link_libraries(
    sw-pico
    pico_stdlib
    pico_multicore
    pico_audio_i2s
    hardware_pio
    hardware_pll
)

# Add any user requested libraries
target_link_libraries(
    sw-pico
    stdio_async_uart
)

pico_add_extra_outputs(sw-pico)

add_custom_target(
    program
    DEPENDS sw-pico
    COMMAND openocd -f interface/picoprobe.cfg -f target/rp2040.cfg -c "program sw-pico.elf verify reset exit"
)
