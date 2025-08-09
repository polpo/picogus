# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
if(NOT PICO_BOARD)
    set(PICO_BOARD "picogus2")
    message("PICO_BOARD not set, defaulting to ${PICO_BOARD}")
endif()
set(PICO_COPY_TO_RAM 1)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)
# to manually set Pico SDK path, uncomment this with your path.
# Otherwise, the environment variable PICO_SDK_PATH will be used.
#set(PICO_SDK_PATH "${CMAKE_SOURCE_DIR}/../../pico/pico-sdk")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(picogus
    VERSION "3.2.0"
    LANGUAGES C CXX ASM
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

include(pico_extras_import.cmake)

#set(PICO_DEOPTIMIZED_DEBUG "1")
#set(CMAKE_BUILD_TYPE "Debug")
set(CMAKE_BUILD_TYPE "Release")
#set(SKIP_PICO_MALLOC 1)

if(NOT PROJECT_TYPE)
    set(PROJECT_TYPE "GUS")
    message("PROJECT_TYPE not set, defaulting to ${PROJECT_TYPE}")
endif()

set(PICO_TINYUSB_PATH ${CMAKE_CURRENT_LIST_DIR}/tinyusb)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

################################################################################
# Helper function for setting linker script
function(set_linker_script TARGET script)
    target_link_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/multifw)
    pico_set_linker_script(${TARGET} ${script})

    # Add dependencies on the 'included' linker scripts so that the target gets
    # rebuilt if they are changed
    pico_add_link_depend(${TARGET} ${script})
endfunction()


# Global definitions across all targets
add_compile_definitions(
    # 370MHz is extremely closely evenly divisible by 44100Hz
    RP2_CLOCK_SPEED=370000
    SYS_CLK_HZ=370000000
    # Manually set up PLLs because of non-default REFDIV
    PLL_SYS_REFDIV=2
    PLL_SYS_VCO_FREQ_HZ=1110000000
    PLL_SYS_POSTDIV1=3
    PLL_SYS_POSTDIV2=1
    # Set voltage before main so no need to do so on the fly
    SYS_CLK_VREG_VOLTAGE_AUTO_ADJUST=1
    SYS_CLK_VREG_VOLTAGE_MIN=VREG_VOLTAGE_1_25
    PICO_DEFAULT_UART=0
    PICO_DEFAULT_UART_TX_PIN=28
    PICO_DEFAULT_UART_RX_PIN=-1
    PICO_DEFAULT_UART_BAUD_RATE=230400
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=250
)

################################################################################
# Helper function for common firmware build
function(config_target TARGET_NAME MULTIFW)
    # Add executable. Default name is the project name, version 0.1
    add_executable(${TARGET_NAME} picogus.cpp)
    pico_set_program_version(${TARGET_NAME} ${CMAKE_PROJECT_VERSION})

    # Add include directory for the new structure
    target_include_directories(${TARGET_NAME} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
    )

    target_compile_definitions(${TARGET_NAME} PRIVATE
        USE_AUDIO_I2S=1
        PICO_AUDIO_I2S_DATA_PIN=16
        PICO_AUDIO_I2S_CLOCK_PIN_BASE=17
        PICO_AUDIO_I2S_PIO=1
        PICO_AUDIO_I2S_SM=3
        M62429_PIO=1
    )

    if(USE_LTO)
        # Pico SDK must be patched to allow for LTO so it is not enabled by default.
        # To patch to Pico SDK, run:
        # sed -i 's/WRAPPER_FUNC(x) __wrap_/WRAPPER_FUNC(x) __attribute__((used)) __wrap_/' "$PICO_SDK_PATH"/src/rp2_common/pico_platform/include/pico/platform.h
        target_compile_options(${TARGET_NAME} PRIVATE -flto=jobserver)
    endif()

    target_sources(${TARGET_NAME} PRIVATE system/pico_reflash.c system/flash_settings.c)
    # target_compile_options(${TARGET_NAME} PRIVATE -save-temps -fverbose-asm)
    if(MULTIFW)
        set_linker_script(${TARGET_NAME} ${CMAKE_BINARY_DIR}/${TARGET_NAME}.ld)
    endif()

    if(USB_JOYSTICK OR USB_MOUSE OR CDROM)
        set(USB_STACK TRUE)
    endif()
    if (USB_STACK)
        # USB stack common stuff
        target_link_libraries(${TARGET_NAME} tinyusb_host)
        target_compile_definitions(${TARGET_NAME} PRIVATE USB_STACK=1)
        target_sources(${TARGET_NAME} PRIVATE usb_hid/hid_app.c)
    endif()
    if(USB_JOYSTICK)
        # Joystick stuff
        target_link_libraries(${TARGET_NAME} hardware_pwm)
        target_include_directories(${TARGET_NAME} PUBLIC ${CMAKE_CURRENT_LIST_DIR}/usb_hid)
        target_link_libraries(${TARGET_NAME} xinput_host)
        target_compile_definitions(${TARGET_NAME} PRIVATE USB_JOYSTICK=1)
    endif()
    if (USB_MOUSE) 
        # USB Mouse stuff
        target_include_directories(${TARGET_NAME} PUBLIC ${CMAKE_CURRENT_LIST_DIR}/mouse)
        target_compile_definitions(${TARGET_NAME} PRIVATE USB_MOUSE=1)
        target_sources(${TARGET_NAME} PRIVATE
            mouse/8250uart.cpp
            mouse/sermouse.cpp
        )
    endif()
    if(CDROM)
        target_link_libraries(${TARGET_NAME} cdrom)
        target_sources(${TARGET_NAME} PRIVATE
            mke/mke.c
            cdrom/msc_app.c
        )
        target_compile_definitions(${TARGET_NAME} PRIVATE CDROM=1)
        target_sources(${TARGET_NAME} PRIVATE
            ${CMAKE_CURRENT_LIST_DIR}/fatfs/source/ff.c
            ${CMAKE_CURRENT_LIST_DIR}/fatfs/source/ffsystem.c
            ${CMAKE_CURRENT_LIST_DIR}/fatfs/source/ffunicode.c
        )
        target_include_directories(${TARGET_NAME} PRIVATE
            ${CMAKE_CURRENT_LIST_DIR}/fatfs/source/
        )
    endif()

    target_sources(${TARGET_NAME} PRIVATE
        M62429/M62429.cpp
        system/pico_pic.c
    )
    pico_generate_pio_header(${TARGET_NAME} ${CMAKE_CURRENT_LIST_DIR}/M62429/M62429.pio)
    pico_generate_pio_header(${TARGET_NAME} ${CMAKE_CURRENT_LIST_DIR}/isa/isa_io.pio)

    if(SOUND_MPU)
        target_sources(${TARGET_NAME} PRIVATE mpu401/mpu401.c mpu401/midi.c)
        target_compile_definitions(${TARGET_NAME} PRIVATE SOUND_MPU=1)
    endif()


    if(SOUND_OPL)
        target_compile_definitions(${TARGET_NAME} PRIVATE
            SOUND_OPL=1
            USE_EMU8950_OPL=1        
            EMU8950_ASM=1
            EMU8950_NO_TLL=1 # don't use lookup table for total level
            EMU8950_NO_FLOAT=1 # double check there is no float
            EMU8950_NO_TIMER=1 # disable timer which isn't used
            EMU8950_NO_TEST_FLAG=1 # disable test flags (which aren't used)
            EMU8950_SIMPLER_NOISE=1 # only generate noise bit when needed
            EMU8950_SHORT_NOISE_UPDATE_CHECK=1 # only update short noise if it is used
        )
        target_link_libraries(${TARGET_NAME} opl)
    endif()

    target_link_libraries(
        ${TARGET_NAME}
        pico_stdlib
        pico_multicore
        pico_audio_i2s
        pico_flash
        hardware_pio
        hardware_pll
        hardware_flash
        hardware_adc
        hardware_exception
    )
    pico_add_extra_outputs(${TARGET_NAME})
    
    if (SOUND_MPU)
        pico_enable_stdio_uart(${TARGET_NAME} 0)
    else()
        pico_enable_stdio_uart(${TARGET_NAME} 1)
    endif()
    if (USB_STACK)
        pico_enable_stdio_usb(${TARGET_NAME} 0)
    else()
        pico_enable_stdio_usb(${TARGET_NAME} 1)
    endif()
    pico_enable_stdio_semihosting(${TARGET_NAME} 0)
endfunction()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/tusb_xinput)
add_subdirectory(rp2040-psram)
add_subdirectory(opl)
add_subdirectory(ne2000)
add_subdirectory(cdrom)
add_subdirectory(resampler)

################################################################################
# Build GUS firmware
function(build_gus TARGET_NAME MULTIFW)
    set(USB_JOYSTICK TRUE)
    set(SOUND_MPU TRUE)
    config_target(${TARGET_NAME} ${MULTIFW})
    pico_set_program_name(${TARGET_NAME} "picogus-gus")
    target_sources(${TARGET_NAME} PRIVATE
        audio/volctrl.cpp
        gusplay.cpp
        isa/isa_dma.c
    )
    target_compile_definitions(${TARGET_NAME} PRIVATE
        SOUND_GUS=1
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
        SCALE_22K_TO_44K=1
        # FORCE_28CH_27CH=1
    )
    pico_generate_pio_header(${TARGET_NAME} ${CMAKE_CURRENT_LIST_DIR}/isa/isa_dma.pio)
    target_link_libraries(${TARGET_NAME} rp2040-psram hardware_interp)
endfunction()

################################################################################
# Build SB firmware
function(build_sb TARGET_NAME MULTIFW)
    set(USB_JOYSTICK TRUE)
    set(SOUND_MPU TRUE)
    set(SOUND_OPL TRUE)
    set(CDROM TRUE)
    config_target(${TARGET_NAME} ${MULTIFW})
    pico_set_program_name(${TARGET_NAME} "picogus-sb")
    target_sources(${TARGET_NAME} PRIVATE
        audio/volctrl.cpp
        sbdsp/sbdsp.cpp
        sbplay.cpp
        isa/isa_dma.c
        audio/audio_fifo.c
        audio/audio_i2s_minimal.c
    )
    target_compile_definitions(${TARGET_NAME} PRIVATE
        SOUND_SB=1
        SOUND_DSP=1
        SB_BUFFERLESS=1
        SB_BUFFERLESS_NG=1
        USE_CD_AUDIO_FIFO=1
        # USE_IRQ=1
        # AUDIO_CALLBACK_CORE0=1
    )
    target_link_libraries(${TARGET_NAME} resampler)
    pico_generate_pio_header(${TARGET_NAME} ${CMAKE_CURRENT_LIST_DIR}/isa/isa_dma.pio)
endfunction()

################################################################################
# Build AdLib firmware
function(build_adlib TARGET_NAME MULTIFW)
    set(USB_JOYSTICK TRUE)
    set(USB_MOUSE TRUE)
    set(SOUND_MPU TRUE)
    set(SOUND_OPL TRUE)
    config_target(${TARGET_NAME} ${MULTIFW})
    pico_set_program_name(${TARGET_NAME} "picogus-adlib")
    target_sources(${TARGET_NAME} PRIVATE
        audio/volctrl.cpp
        sbplay.cpp
        audio/audio_i2s_minimal.c
    )  
    target_link_libraries(${TARGET_NAME} resampler)
endfunction()

################################################################################
# Build MPU-only firmware
function(build_mpu TARGET_NAME MULTIFW)
    set(USB_JOYSTICK TRUE)
    set(SOUND_MPU TRUE)
    config_target(${TARGET_NAME} ${MULTIFW})
    pico_set_program_name(${TARGET_NAME} "picogus-mpu401")
    target_compile_definitions(${TARGET_NAME} PRIVATE
        # Enable MPU-401 IRQ support only in this firmware
        MPU_IRQ=1
        # Larger buffer for sysex delay
        MPU_ONLY=1
    )
    target_sources(${TARGET_NAME} PRIVATE mpuplay.cpp)
endfunction()

################################################################################
# Build PSG firmware
function(build_psg TARGET_NAME MULTIFW)
    set(USB_JOYSTICK TRUE)
    set(USB_MOUSE TRUE)
    set(SOUND_MPU TRUE)
    config_target(${TARGET_NAME} ${MULTIFW})
    pico_set_program_name(${TARGET_NAME} "picogus-psg")
    target_compile_definitions(${TARGET_NAME} PRIVATE
        SOUND_TANDY=1
        SOUND_CMS=1
    )
    target_sources(${TARGET_NAME} PRIVATE
        audio/volctrl.cpp
        square/square.cpp
    )
    target_sources(${TARGET_NAME} PRIVATE psgplay.cpp)
endfunction()

################################################################################
# Build USB-only firmware (joystick + mouse support)
function(build_usb TARGET_NAME MULTIFW)
    set(USB_JOYSTICK TRUE)
    set(USB_MOUSE TRUE)
    set(SOUND_MPU TRUE)
    set(CDROM TRUE)
    config_target(${TARGET_NAME} ${MULTIFW})
    pico_set_program_name(${TARGET_NAME} "picogus-usb")
    target_compile_definitions(${TARGET_NAME} PRIVATE
        USB_ONLY=1
    )
    target_sources(${TARGET_NAME} PRIVATE 
        audio/volctrl.cpp
        usbplay.cpp
    )
endfunction()


function(build_ne2k TARGET_NAME MULTIFW)
    config_target(${TARGET_NAME} ${MULTIFW})
    pico_set_program_name(${TARGET_NAME} "picogus-ne2k")
    target_compile_definitions(${TARGET_NAME} PRIVATE
       NE2000=1
       PICOW=1
       CYW43_CONFIG_FILE="cyw43_firmware.h"
       CYW43_PIO_CLOCK_DIV_INT=3
    )
    target_sources(${TARGET_NAME} PRIVATE
        ne2000play.cpp
    )
    target_link_libraries(
        ${TARGET_NAME}
        ne2000        
    )
endfunction()


################################################################################
# Build "multifw" - all firmwares in one UF2
if(PROJECT_TYPE STREQUAL "MULTIFW")
    set(FW_TARGET picogus)
    add_custom_command(
        OUTPUT
            "${CMAKE_BINARY_DIR}/generated/multifw/flash_firmware.h"
            "${CMAKE_BINARY_DIR}/pg-gus-multifw.ld"
            "${CMAKE_BINARY_DIR}/pg-adlib-multifw.ld"
            "${CMAKE_BINARY_DIR}/pg-mpu-multifw.ld"
            "${CMAKE_BINARY_DIR}/pg-psg-multifw.ld"
            "${CMAKE_BINARY_DIR}/pg-sb-multifw.ld"
            "${CMAKE_BINARY_DIR}/pg-usb-multifw.ld"
        COMMAND
            ${CMAKE_COMMAND}
            -DFLASH_FIRMWARE_H_IN="${CMAKE_SOURCE_DIR}/multifw/flash_firmware.h.in"
            -DFLASH_FIRMWARE_H="${CMAKE_BINARY_DIR}/generated/multifw/flash_firmware.h"
            -DFW_LD_IN="${CMAKE_SOURCE_DIR}/multifw/firmware.ld.in"
            -DFW_1_BIN="${CMAKE_BINARY_DIR}/pg-gus.bin"
            -DFW_1_LD="${CMAKE_BINARY_DIR}/pg-gus-multifw.ld"
            -DFW_2_BIN="${CMAKE_BINARY_DIR}/pg-adlib.bin"
            -DFW_2_LD="${CMAKE_BINARY_DIR}/pg-adlib-multifw.ld"
            -DFW_3_BIN="${CMAKE_BINARY_DIR}/pg-mpu.bin"
            -DFW_3_LD="${CMAKE_BINARY_DIR}/pg-mpu-multifw.ld"
            -DFW_4_BIN="${CMAKE_BINARY_DIR}/pg-psg.bin"
            -DFW_4_LD="${CMAKE_BINARY_DIR}/pg-psg-multifw.ld"
            -DFW_5_BIN="${CMAKE_BINARY_DIR}/pg-sb.bin"
            -DFW_5_LD="${CMAKE_BINARY_DIR}/pg-sb-multifw.ld"
            -DFW_6_BIN="${CMAKE_BINARY_DIR}/pg-usb.bin"
            -DFW_6_LD="${CMAKE_BINARY_DIR}/pg-usb-multifw.ld"
            -P ${CMAKE_SOURCE_DIR}/multifw/generate_ld.cmake
        DEPENDS
            "${CMAKE_SOURCE_DIR}/multifw/firmware.ld.in"
            pg-gus
            pg-adlib
            pg-mpu
            pg-psg
            pg-sb
            pg-usb
    )
    add_custom_target(generate_flash_firmware_h DEPENDS
        "${CMAKE_BINARY_DIR}/generated/multifw/flash_firmware.h"
        "${CMAKE_BINARY_DIR}/pg-gus-multifw.ld"
        "${CMAKE_BINARY_DIR}/pg-adlib-multifw.ld"
        "${CMAKE_BINARY_DIR}/pg-mpu-multifw.ld"
        "${CMAKE_BINARY_DIR}/pg-psg-multifw.ld"
        "${CMAKE_BINARY_DIR}/pg-sb-multifw.ld"
        "${CMAKE_BINARY_DIR}/pg-usb-multifw.ld"
    )

    build_gus(pg-gus FALSE)
    build_gus(pg-gus-multifw TRUE)
    add_dependencies(pg-gus-multifw pg-gus generate_flash_firmware_h)
    build_adlib(pg-adlib FALSE)
    build_adlib(pg-adlib-multifw TRUE)
    add_dependencies(pg-adlib-multifw pg-adlib generate_flash_firmware_h)
    build_mpu(pg-mpu FALSE)
    build_mpu(pg-mpu-multifw TRUE)
    add_dependencies(pg-mpu-multifw pg-mpu generate_flash_firmware_h)
    build_psg(pg-psg FALSE)
    build_psg(pg-psg-multifw TRUE)
    add_dependencies(pg-psg-multifw pg-psg generate_flash_firmware_h)
    build_sb(pg-sb FALSE)
    build_sb(pg-sb-multifw TRUE)
    add_dependencies(pg-sb-multifw pg-sb generate_flash_firmware_h)
    build_usb(pg-usb FALSE)
    build_usb(pg-usb-multifw TRUE)
    add_dependencies(pg-usb-multifw pg-usb generate_flash_firmware_h)

    add_executable(bootloader)
    target_sources(bootloader PRIVATE multifw/bootloader.c system/flash_settings.c)
    target_link_libraries(bootloader PRIVATE pico_stdlib hardware_flash pico_flash hardware_exception)
    pico_add_extra_outputs(bootloader)
    # set_linker_script(bootloader ${CMAKE_SOURCE_DIR}/multifw/bootloader.ld)
    target_include_directories(bootloader PRIVATE 
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_BINARY_DIR}/generated/multifw
    )
    pico_enable_stdio_uart(bootloader 0)
    pico_enable_stdio_usb(bootloader 0)
    pico_enable_stdio_semihosting(bootloader 0)
    add_dependencies(bootloader generate_flash_firmware_h)

    if (NOT UF2CREATE_FOUND)
        set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/multifw)
        # Needs to be an external project so it can be built as a native binary outside of the Pico SDK
        find_package(UF2CREATE)
        # Rebuild uf2create if flash_firmware.h is regenerated
        ExternalProject_Add_StepDependencies(UF2CREATEBuild build generate_flash_firmware_h)
    endif()
    add_custom_command(
        OUTPUT picogus.uf2
        DEPENDS bootloader UF2CREATE pg-gus-multifw pg-adlib-multifw pg-mpu-multifw pg-psg-multifw pg-sb-multifw pg-usb-multifw
        COMMAND UF2CREATE bootloader.bin pg-gus-multifw.bin pg-adlib-multifw.bin pg-mpu-multifw.bin pg-psg-multifw.bin pg-sb-multifw.bin pg-usb-multifw.bin picogus.uf2
    )
    add_custom_target(picogus ALL DEPENDS picogus.uf2)

################################################################################
# Builds for individual "legacy" firmware modes
elseif(PROJECT_TYPE STREQUAL "GUS")
    set(FW_TARGET pg-gus)
    build_gus(pg-gus FALSE)
elseif(PROJECT_TYPE STREQUAL "SB")
    set(FW_TARGET pg-sb)
    build_sb(pg-sb FALSE)
elseif(PROJECT_TYPE STREQUAL "ADLIB")
    set(FW_TARGET pg-adlib)
    build_adlib(pg-adlib FALSE)
elseif(PROJECT_TYPE STREQUAL "MPU")
    set(FW_TARGET pg-mpu)
    build_mpu(pg-mpu FALSE)
elseif(PROJECT_TYPE STREQUAL "PSG")
    set(FW_TARGET pg-psg)
    build_psg(pg-psg FALSE)
elseif(PROJECT_TYPE STREQUAL "USB")
    set(FW_TARGET pg-usb)
    build_usb(pg-usb FALSE)
elseif(PROJECT_TYPE STREQUAL "NE2K")
    set(FW_TARGET pg-ne2k)
    build_ne2k(pg-ne2k FALSE)
endif()

################################################################################
# Optional target to program built firmware via openocd
add_custom_target(
    program
    DEPENDS ${FW_TARGET}
    COMMAND openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg -c "adapter speed 5000\; program ${FW_TARGET}.bin verify reset exit 0x10000000"
)
