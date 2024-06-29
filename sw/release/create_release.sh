#!/bin/bash

set -e

RELEASE_DIR="$( dirname -- "$( readlink -f -- "$0"; )"; )"
SW_HOME="$(readlink -f "$RELEASE_DIR"/..)"
BUILD_DIR="$SW_HOME"/build
STAGING_DIR="$SW_HOME"/release/staging_dir

mkdir -p "$BUILD_DIR"
mkdir -p "$STAGING_DIR"

rm -f "$STAGING_DIR"/*

build () {
    local PROJECT_TYPE=$1
    local FW_SUFFIX=$2
    local EXTRA_CMAKE="$3"
    local EXTRA_BUILD="$4"

    cd "$BUILD_DIR"
#    cmake .. --fresh -DPROJECT_TYPE=$PROJECT_TYPE $EXTRA_CMAKE
    cmake .. -DPROJECT_TYPE=$PROJECT_TYPE $EXTRA_CMAKE
    cmake --build . -j $(nproc) $EXTRA_BUILD
    if [[ ! $2 == 'BOOT' ]]; then
        if [[ $FW_SUFFIX ]]; then
            cp picogus.bin "$STAGING_DIR"/pg-$FW_SUFFIX.bin
            cp picogus.uf2 "$STAGING_DIR"/pg-$FW_SUFFIX.uf2
        fi
    else
        cp bootloader.bin "$STAGING_DIR"/bootloader.bin 
        gcc ../uf2create.c -o uf2create
    fi
    cd -
}

# Build pgusinit
cd "$SW_HOME"/../pgusinit
export WATCOM="$(readlink -f "$SW_HOME"/../../watcom)"
# export WATCOM="/usr/bin/watcom"
if [[ $OSTYPE == 'darwin'* ]]; then
    # Assuming Apple ARM 
    BINDIR="$WATCOM"/armo64
else
    # Otherwise, Linux x64
    BINDIR="$WATCOM"/binl64
fi
export PATH="$BINDIR":"$PATH"
export INCLUDE="$WATCOM"/h
make -f Makefile-cross clean
make -f Makefile-cross
cp pgusinit.exe "$STAGING_DIR"
cp README.md "$STAGING_DIR"
cd -

# Build picogus releases
BOOTLOADER_SIZE=32768
build GUS "" "--fresh" "--clean-first"
FW_1_SIZE=$((($(wc -c <"$BUILD_DIR"/picogus.bin) / 1024) * 1024 + 1024))
export FW_1_ORIGIN=$BOOTLOADER_SIZE
FW_SIZE=$FW_1_SIZE FW_ORIGIN=$FW_1_ORIGIN envsubst <../firmware.ld.in >../firmware.ld
build GUS "gus" "--fresh -D MULTIFW=1" "--clean-first"

build SB "" "--fresh" "--clean-first"
FW_2_SIZE=$((($(wc -c <"$BUILD_DIR"/picogus.bin) / 1024) * 1024 + 1024))
export FW_2_ORIGIN=$(($FW_1_ORIGIN + $FW_1_SIZE))
FW_SIZE=$FW_2_SIZE FW_ORIGIN=$FW_2_ORIGIN envsubst <../firmware.ld.in >../firmware.ld
build SB "sb" "--fresh -D MULTIFW=1" "--clean-first"

build MPU "" "--fresh" "--clean-first"
FW_3_SIZE=$((($(wc -c <"$BUILD_DIR"/picogus.bin) / 1024) * 1024 + 1024))
export FW_3_ORIGIN=$(($FW_2_ORIGIN + $FW_2_SIZE))
FW_SIZE=$FW_3_SIZE FW_ORIGIN=$FW_3_ORIGIN envsubst <../firmware.ld.in >../firmware.ld
build MPU "mpu" "--fresh -D MULTIFW=1" "--clean-first"

build TANDY "" "--fresh" "--clean-first"
FW_4_SIZE=$((($(wc -c <"$BUILD_DIR"/picogus.bin) / 1024) * 1024 + 1024))
export FW_4_ORIGIN=$(($FW_3_ORIGIN + $FW_3_SIZE))
FW_SIZE=$FW_4_SIZE FW_ORIGIN=$FW_4_ORIGIN envsubst <../firmware.ld.in >../firmware.ld
build TANDY "tandy" "--fresh -D MULTIFW=1" "--clean-first"

build CMS "" "--fresh" "--clean-first"
FW_5_SIZE=$((($(wc -c <"$BUILD_DIR"/picogus.bin) / 1024) * 1024 + 1024))
export FW_5_ORIGIN=$(($FW_4_ORIGIN + $FW_4_SIZE))
FW_SIZE=$FW_5_SIZE FW_ORIGIN=$FW_5_ORIGIN envsubst <../firmware.ld.in >../firmware.ld
build CMS "cms" "--fresh -D MULTIFW=1" "--clean-first"

build JOY "" "--fresh" "--clean-first"
FW_6_SIZE=$((($(wc -c <"$BUILD_DIR"/picogus.bin) / 1024) * 1024 + 1024))
export FW_6_ORIGIN=$(($FW_5_ORIGIN + $FW_5_SIZE))
FW_SIZE=$FW_6_SIZE FW_ORIGIN=$FW_6_ORIGIN envsubst <../firmware.ld.in >../firmware.ld
build JOY "joy" "--fresh -D MULTIFW=1" "--clean-first"

envsubst < ../flash_firmware.h.in > ../flash_firmware.h
build BOOT "BOOT" "--fresh" "--clean-first" # BOOTLOADER for multifw

# Get release version
cd "$BUILD_DIR"
PICOGUS_VERSION=$(cmake --system-information | awk -F= '$1~/CMAKE_PROJECT_VERSION:STATIC/{print$2}')
cd -

#bin files for multi-firmware
cd "$STAGING_DIR"
"$BUILD_DIR"/uf2create bootloader.bin pg-gus.bin pg-sb.bin pg-mpu.bin pg-tandy.bin pg-cms.bin pg-joy.bin pg-multi.uf2
# rm *.bin
cd -

# Create zip file
rm -f picogus-$PICOGUS_VERSION.zip
cd "$STAGING_DIR"
zip -9 ../picogus-$PICOGUS_VERSION.zip *
