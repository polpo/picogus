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

    cd "$BUILD_DIR"
#    cmake .. --fresh -DPROJECT_TYPE=$PROJECT_TYPE $EXTRA_CMAKE
    cmake --clean-first .. -DPROJECT_TYPE=$PROJECT_TYPE $EXTRA_CMAKE
    make clean && make -j
    if [[ ! $2 == 'BOOT' ]]; then
        cp picogus.bin "$STAGING_DIR"/pg-$FW_SUFFIX.bin
        cp picogus.uf2 "$STAGING_DIR"/pg-$FW_SUFFIX.uf2
        cd -
    else
        cp bootloader.bin "$STAGING_DIR"/bootloader.bin 
        gcc ../uf2create.c -o uf2create
    fi
}

# Build pgusinit
cd "$SW_HOME"/../pgusinit
#export WATCOM="$(readlink -f "$SW_HOME"/../../watcom)"
export WATCOM="/usr/bin/watcom"
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
build BOOT "BOOT"  # BOOTLOADER for multifw
build GUS "gus" "-DGUS_DEFAULT_PORT=0x240"
build SB "sb"
build MPU "mpu"
build TANDY "tandy"
build CMS "cms"
build JOY "joy"

# Get release version
cd "$BUILD_DIR"
PICOGUS_VERSION=$(cmake --system-information | awk -F= '$1~/CMAKE_PROJECT_VERSION:STATIC/{print$2}')
cd -

#bin files for multi-firmware
cd "$STAGING_DIR"
"$BUILD_DIR"/uf2create bootloader.bin pg-gus.bin pg-sb.bin pg-mpu.bin pg-tandy.bin pg-cms.bin pg-joy.bin pg-multi.uf2
rm *.bin
cd -

# Create zip file
rm -f picogus-$PICOGUS_VERSION.zip
cd "$STAGING_DIR"
zip -9 ../picogus-$PICOGUS_VERSION.zip *
