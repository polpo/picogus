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
    VERSION "1.0.1"
    LANGUAGES C CXX ASM
)

set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR})
set(PICO_BOARD picogus2)

# set(PICO_DEOPTIMIZED_DEBUG "1")
set(CMAKE_BUILD_TYPE "Release")
set(PICO_COPY_TO_RAM 1)
#set(SKIP_PICO_MALLOC 1)

# Tell the Pico SDK to use our local tinyusb instead of the one included in the SDK
# This is needed to support tusb_xinput
set(PICO_TINYUSB_PATH ${CMAKE_CURRENT_LIST_DIR}/tinyusb)

if(NOT PROJECT_TYPE)
    set(PROJECT_TYPE "GUS")
    message("PROJECT_TYPE not set, defaulting to ${PROJECT_TYPE}")
endif()

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

if(USE_LTO)
    # Pico SDK must be patched to allow for LTO so it is not enabled by default.
    # To patch to Pico SDK, run:
    # sed -i 's/WRAPPER_FUNC(x) __wrap_/WRAPPER_FUNC(x) __attribute__((used)) __wrap_/' "$PICO_SDK_PATH"/src/rp2_common/pico_platform/include/pico/platform.h
    add_compile_options(-flto=jobserver)
endif()

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
        PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=250
        M62429_PIO=1
)

target_sources(picogus PRIVATE pico_reflash.c)

target_compile_definitions(picogus PRIVATE PICO_FLASH_SPI_CLKDIV=4)

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
        TEST_PSRAM=1
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
    pico_enable_stdio_uart(picogus 1)
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
    # Use USB stdio because UART is used by MIDI, but only if USB joysick is disabled
    pico_enable_stdio_uart(picogus 0)
    if(USB_JOYSTICK)
        pico_enable_stdio_usb(picogus 0)
    else()
        pico_enable_stdio_usb(picogus 1)
    endif()
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
    set(USB_JOYSTICK 1)
    target_compile_definitions(picogus PRIVATE
        USB_JOYSTICK_ONLY=1
    )
 
    target_sources(picogus PRIVATE
        usbplay.cpp
    )
     pico_enable_stdio_uart(picogus 1)
     pico_enable_stdio_usb(picogus 0)
     pico_enable_stdio_semihosting(picogus 0)
endif()

if(USB_JOYSTICK)
    # Joystick stuff
    target_sources(picogus PRIVATE joy_hid/joy_hid.c)
    target_link_libraries(picogus hardware_pwm)
    target_include_directories(picogus PUBLIC ${CMAKE_CURRENT_LIST_DIR}/joy_hid)
    target_link_libraries(picogus tinyusb_host)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/tusb_xinput)
    target_link_libraries(picogus xinput_host_app_driver)
    target_compile_definitions(picogus PRIVATE USB_JOYSTICK=1)
endif()

target_sources(picogus PRIVATE M62429/M62429.cpp)
pico_generate_pio_header(picogus ${CMAKE_CURRENT_LIST_DIR}/M62429/M62429.pio)

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
    hardware_adc
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
