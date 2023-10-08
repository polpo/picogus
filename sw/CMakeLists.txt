# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)
# to manually set Pico SDK path, uncomment this with your path.
# Otherwise, the environment variable PICO_SDK_PATH will be used.
#set(PICO_SDK_PATH "${CMAKE_SOURCE_DIR}/../../pico/pico-sdk")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)
include(pico_extras_import.cmake)

project(picogus
    VERSION "0.7.0"
    LANGUAGES C CXX ASM
)

# set(PICO_DEOPTIMIZED_DEBUG "1")
set(CMAKE_BUILD_TYPE "Release")
set(PICO_COPY_TO_RAM 1)
#set(SKIP_PICO_MALLOC 1)

if(NOT PROJECT_TYPE)
    set(PROJECT_TYPE "GUS")
    message("PROJECT_TYPE not set, defaulting to ${PROJECT_TYPE}")
endif()

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

add_compile_options(-flto=jobserver)

# Add executable. Default name is the project name, version 0.1
add_executable(picogus picogus.cpp)
pico_set_program_version(picogus ${CMAKE_PROJECT_VERSION})

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

target_sources(picogus PRIVATE pico_reflash.c)

if(PROJECT_TYPE STREQUAL "GUS")
    # target_compile_options(picogus PRIVATE -save-temps -fverbose-asm)
    if(NOT GUS_DEFAULT_PORT)
        set(GUS_DEFAULT_PORT "0x240")
        message("GUS_DEFAULT_PORT not set, defaulting to ${GUS_DEFAULT_PORT}")
    endif()
    pico_set_program_name(picogus "picogus-gus")
    target_sources(picogus PRIVATE gusplay.cpp pico_pic.c isa_dma.c)
    target_compile_definitions(picogus PRIVATE
        SOUND_GUS=1
        GUS_DEFAULT_PORT=${GUS_DEFAULT_PORT}
        PSRAM=1
        #TEST_PSRAM=1
        PSRAM_ASYNC=1
        #PSRAM_ASYNC_DMA_IRQ=1
        PSRAM_SPINLOCK=1
        # PSRAM_MUTEX=1
        # PSRAM_WAITDMA=1
        PSRAM_CORE0=1
        PSRAM_PIN_CS=1
        PSRAM_PIN_SCK=2
        PSRAM_PIN_MOSI=3
        PSRAM_PIN_MISO=0
        # POLLING_DMA=1
        INTERP_CLAMP=1
        # INTERP_LINEAR=1
    )
    add_subdirectory(rp2040-psram)
    target_link_libraries(picogus rp2040-psram hardware_interp)
    # pico_generate_pio_header(picogus ${CMAKE_CURRENT_LIST_DIR}/psram_spi.pio)
    set(ASYNC_UART 0)
    if(ASYNC_UART)
        add_compile_definitions(
            ASYNC_UART=1
        )
        pico_enable_stdio_uart(picogus 0)
    else()
        pico_enable_stdio_uart(picogus 1)
    endif()
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
    )
    target_sources(picogus PRIVATE
        pico_pic.c
        mpu401/mpu401.c
        mpu401/midi.c
    )
    target_sources(picogus PRIVATE mpuplay.cpp)
    # Use USB stdio because UART is used by MIDI
    pico_enable_stdio_uart(picogus 0)
    pico_enable_stdio_usb(picogus 0)
    pico_enable_stdio_semihosting(picogus 0)
endif()

if(PROJECT_TYPE STREQUAL "TANDY")
    pico_set_program_name(picogus "picogus-tandy")
    target_compile_definitions(picogus PRIVATE
        SOUND_TANDY=1
        # SQUARE_FLOAT_OUTPUT=1
    )
    target_sources(picogus PRIVATE
        pico_pic.c
        square/square.cpp
    )
    target_sources(picogus PRIVATE tandyplay.cpp)
    pico_enable_stdio_uart(picogus 1)
    pico_enable_stdio_usb(picogus 0)
    pico_enable_stdio_semihosting(picogus 0)
endif()

if(PROJECT_TYPE STREQUAL "CMS")
    pico_set_program_name(picogus "picogus-cms")
    target_compile_definitions(picogus PRIVATE
        SOUND_CMS=1
        # SQUARE_FLOAT_OUTPUT=1
    )

    target_sources(picogus PRIVATE
        pico_pic.c
        square/square.cpp
    )
    target_sources(picogus PRIVATE cmsplay.cpp)
    pico_enable_stdio_uart(picogus 1)
    pico_enable_stdio_usb(picogus 0)
    pico_enable_stdio_semihosting(picogus 0)
endif()

if(PROJECT_TYPE STREQUAL "JOY")
    pico_set_program_name(picogus "picogus-joy")
    target_compile_definitions(picogus PRIVATE
        USB_JOYSTICK=1
    )

    target_sources(picogus PRIVATE
        joy_hid/joy_hid.c
        usbplay.cpp
    )
    pico_enable_stdio_uart(picogus 1)
    pico_enable_stdio_usb(picogus 0)
    pico_enable_stdio_semihosting(picogus 0)

    # Make sure TinyUSB can find tusb_config.h
    target_include_directories(picogus PUBLIC
            ${CMAKE_CURRENT_LIST_DIR}/joy_hid)

    # In addition to pico_stdlib required for common PicoSDK functionality, add dependency on tinyusb_host
    # for TinyUSB device support and tinyusb_board for the additional board support library used by the example
    target_link_libraries(picogus tinyusb_host tinyusb_board hardware_pwm)
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
    hardware_flash
)

if(ASYNC_UART)
    add_subdirectory(stdio_async_uart)
    target_link_libraries(
        picogus
        stdio_async_uart
    )
endif()

pico_add_extra_outputs(picogus)
# print size
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND arm-none-eabi-size --format=berkeley "${PROJECT_NAME}.elf")

add_custom_target(
    program
    DEPENDS picogus
    COMMAND openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg -c "adapter speed 5000\; program picogus.elf verify reset exit"
)
