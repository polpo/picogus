# Script to generate the linker scripts for each firmware with the firmware size and offset (aligned to 1024 bytes)
# As well as the flash_firmware.h file used by the bootloader and uf2create utility

# Initial fixed offset of 32768 for the bootloader
set(FW_ORIGIN "32768")

macro(generate_ld_file FW_FILE FW_LD_FILE)
    file(SIZE ${FW_FILE} FW_SIZE)
    math(EXPR FW_SIZE_ALIGNED "(${FW_SIZE} / 1024) * 1024 + 1024")
    # file(REMOVE ${FW_LD_FILE})
    configure_file(${FW_LD_IN} ${FW_LD_FILE})
    list(APPEND FW_ORIGINS "${FW_ORIGIN}")
    math(EXPR FW_ORIGIN "${FW_ORIGIN} + ${FW_SIZE_ALIGNED}")
endmacro()

generate_ld_file(${FW_1_BIN} ${FW_1_LD})
generate_ld_file(${FW_2_BIN} ${FW_2_LD})
generate_ld_file(${FW_3_BIN} ${FW_3_LD})
generate_ld_file(${FW_4_BIN} ${FW_4_LD})
generate_ld_file(${FW_5_BIN} ${FW_5_LD})
generate_ld_file(${FW_6_BIN} ${FW_6_LD})
generate_ld_file(${FW_7_BIN} ${FW_7_LD})
list(GET FW_ORIGINS 0 FW_1_ORIGIN)
list(GET FW_ORIGINS 1 FW_2_ORIGIN)
list(GET FW_ORIGINS 2 FW_3_ORIGIN)
list(GET FW_ORIGINS 3 FW_4_ORIGIN)
list(GET FW_ORIGINS 4 FW_5_ORIGIN)
list(GET FW_ORIGINS 5 FW_6_ORIGIN)
list(GET FW_ORIGINS 6 FW_7_ORIGIN)

file(REMOVE ${FLASH_FIRMWARE_H})
configure_file(${FLASH_FIRMWARE_H_IN} ${FLASH_FIRMWARE_H})
