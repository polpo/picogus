# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)
set(PICO_SDK_PATH "${CMAKE_SOURCE_DIR}/../../pico/pico-sdk")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)
include(pico_extras_import.cmake)

project(picogus C CXX ASM)

# set(PICO_DEOPTIMIZED_DEBUG "1")
set(CMAKE_BUILD_TYPE "Release")
set(PICO_COPY_TO_RAM 1)
#set(SKIP_PICO_MALLOC 1)

if(NOT PROJECT_TYPE)
    set(PROJECT_TYPE "GUS")
endif()

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1
add_executable(picogus picogus.cpp)
pico_set_program_version(picogus "0.1.0")

add_compile_definitions(
        USE_AUDIO_I2S=1
        PICO_AUDIO_I2S_DATA_PIN=16
        PICO_AUDIO_I2S_CLOCK_PIN_BASE=17
        PICO_DEFAULT_UART=0
        PICO_DEFAULT_UART_TX_PIN=28
        PICO_DEFAULT_UART_RX_PIN=-1
        PICO_AUDIO_I2S_PIO=1
        # USE_IRQ=1
        USE_ALARM=1
        PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=5000000
)

if(PROJECT_TYPE STREQUAL "GUS")
    if(NOT GUS_PORT)
        set(GUS_PORT "0x240")
    endif()
    pico_set_program_name(picogus "picogus-gus")
    target_sources(picogus PRIVATE gusplay.cpp pico_pic.c)
    target_compile_definitions(picogus PRIVATE
        SOUND_GUS=1
        PSRAM=1
        GUS_PORT=${GUS_PORT}
        # TEST_PSRAM=1
        # USE_SPINLOCK=1
    )
    pico_generate_pio_header(picogus ${CMAKE_CURRENT_LIST_DIR}/psram_spi.pio)
    pico_enable_stdio_uart(picogus 0)
    pico_enable_stdio_usb(picogus 0)
    pico_enable_stdio_semihosting(picogus 0)
endif()

if(PROJECT_TYPE STREQUAL "OPL")
    pico_set_program_name(picogus "picogus-adlib")
    target_sources(picogus PRIVATE adlibplay.cpp)
    target_link_libraries(
        picogus
        opl
    )
    target_compile_definitions(picogus PRIVATE
        SOUND_OPL=1
        USE_EMU8950_OPL=1
        EMU8950_ASM=1
    )
    add_subdirectory(opl)
    pico_enable_stdio_uart(picogus 0)
    pico_enable_stdio_usb(picogus 0)
    pico_enable_stdio_semihosting(picogus 0)
endif()

if(PROJECT_TYPE STREQUAL "MPU")
    pico_set_program_name(picogus "picogus-mpu401")
    target_compile_definitions(picogus PRIVATE
        SOUND_MPU=1
        USE_ALARM=1
    )
    target_sources(picogus PRIVATE
        pico_pic.c
        mpu401/mpu401.c
        mpu401/midi.c
    )
    # Use USB stdio because UART is used by MIDI
    pico_enable_stdio_uart(picogus 0)
    pico_enable_stdio_usb(picogus 1)
    pico_enable_stdio_semihosting(picogus 0)
endif()

pico_generate_pio_header(picogus ${CMAKE_CURRENT_LIST_DIR}/isa_io.pio)
pico_generate_pio_header(picogus ${CMAKE_CURRENT_LIST_DIR}/isa_dma.pio)

target_link_libraries(
    picogus
    pico_stdlib
    pico_multicore
    pico_audio_i2s
    hardware_pio
    hardware_pll
)

add_subdirectory(stdio_async_uart)
target_link_libraries(
    picogus
    stdio_async_uart
)

pico_add_extra_outputs(picogus)

add_custom_target(
    program
    DEPENDS picogus
    COMMAND openocd -f interface/picoprobe.cfg -f target/rp2040.cfg -c "program picogus.elf verify reset exit"
)

unset(PROJECT_TYPE CACHE)
unset(GUS_PORT CACHE)
