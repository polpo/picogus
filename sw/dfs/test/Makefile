# Host test harness for the PGDFS file server (../dfs_server.c, ../dfs_fs.c).
#
#   make test            build with cc and run the server tests
#   make test DEBUG=1    same, with DFS_LOG() output from the server
#   make arm-size        compile the firmware objects with arm-none-eabi-gcc
#                        (firmware ffconf.h) and print their sizes
#   make clean
#
# FatFs resolves #include "ffconf.h" relative to the directory ff.h lives in
# (clang follows symlinks to the real directory), so no -I order can swap in
# a host configuration. The build directory therefore holds copies of
# ff.c/ff.h/diskio.h/ffunicode.c next to a copy of ffconf_host.h named
# ffconf.h (firmware config + FF_USE_MKFS 1).

CC      ?= cc
B       := build
DFS     := ..
SW      := ../..
FATFS   := $(SW)/fatfs/source
CFLAGS  ?= -O1 -g
WARN    := -Wall -Wextra -Wno-unused-parameter
INC     := -I$(B) -I$(DFS) -I$(SW) -I$(SW)/../common
ifdef DEBUG
CFLAGS  += -DDFS_DEBUG
endif

LINKS := $(B)/ffconf.h $(B)/ff.c $(B)/ff.h $(B)/diskio.h $(B)/ffunicode.c
OBJS  := $(B)/dfs_server.o $(B)/dfs_fs.o $(B)/ff.o $(B)/ffunicode.o \
         $(B)/ramdisk_diskio.o $(B)/test_server.o
HDRS  := $(DFS)/dfs_fs.h $(DFS)/dfs_server.h $(SW)/../common/picogus.h $(SW)/usb_msc/msc_app.h ramdisk_diskio.h

.PHONY: all test clean arm-size

all: $(B)/test_server

test: $(B)/test_server
	./$(B)/test_server

$(B):
	mkdir -p $(B)

$(B)/ffconf.h: ffconf_host.h | $(B)
	cp $< $@

$(B)/ff.c $(B)/ff.h $(B)/diskio.h $(B)/ffunicode.c: $(B)/%: $(FATFS)/% | $(B)
	cp $< $@

$(B)/ff.o: $(B)/ff.c $(LINKS)
	$(CC) $(CFLAGS) -Wall $(INC) -c $< -o $@

$(B)/ffunicode.o: $(B)/ffunicode.c $(LINKS)
	$(CC) $(CFLAGS) -Wall $(INC) -c $< -o $@

$(B)/%.o: $(DFS)/%.c $(LINKS) $(HDRS)
	$(CC) $(CFLAGS) $(WARN) $(INC) -c $< -o $@

$(B)/%.o: %.c $(LINKS) $(HDRS)
	$(CC) $(CFLAGS) $(WARN) $(INC) -c $< -o $@

$(B)/test_server: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

# ---- RP2040 compile-only check with the firmware FatFs configuration --------
ARMCC    ?= arm-none-eabi-gcc
ARMSIZE  ?= arm-none-eabi-size
ARMFLAGS := -mcpu=cortex-m0plus -mthumb -Os -Wall
ARMINC   := -I$(FATFS) -I$(DFS) -I$(SW) -I$(SW)/../common
ARMOBJS  := $(B)/arm/dfs_server.o $(B)/arm/dfs_fs.o $(B)/arm/ff.o

arm-size: $(ARMOBJS)
	$(ARMSIZE) $(ARMOBJS)

$(B)/arm:
	mkdir -p $@

$(B)/arm/%.o: $(DFS)/%.c $(HDRS) | $(B)/arm
	$(ARMCC) $(ARMFLAGS) -Wextra -Wno-unused-parameter $(ARMINC) -c $< -o $@

$(B)/arm/ff.o: $(FATFS)/ff.c $(FATFS)/ffconf.h | $(B)/arm
	$(ARMCC) $(ARMFLAGS) $(ARMINC) -c $< -o $@

clean:
	rm -rf $(B)
