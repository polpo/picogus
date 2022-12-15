#!/bin/bash

set -e

if [ -z $PICOGUS_VERSION ]; then
    echo 'Please set the environment variable $PICOGUS_VERSION'
    exit 1
fi

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
    cmake .. -DPROJECT_TYPE=$PROJECT_TYPE $EXTRA_CMAKE
    make clean && make -j
    cp picogus.uf2 "$STAGING_DIR"/picogus-$FW_SUFFIX.uf2
}

# Build pgusinit
cd "$SW_HOME"/../pgusinit
export WATCOM="$(readlink -f "$SW_HOME"/../../watcom)"
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
cd -

# Build picogus releases
build GUS "gus_220" "-DGUS_PORT=0x220"
build GUS "gus_240" "-DGUS_PORT=0x240"
build GUS "gus_260" "-DGUS_PORT=0x260"
build OPL "adlib"
build MPU "mpu401"

# Create zip file
cd "$STAGING_DIR"
zip -9 ../picogus-$PICOGUS_VERSION.zip *
