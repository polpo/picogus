# Host test harness for the CD image manager (../cdrom_image_manager.c), the
# path helpers (../cdrom_path.c) and the cue sheet loader
# (../cdrom_image_backend.c).
#
#   make test            build with cc and run the tests
#   make test SAN=1      same, under AddressSanitizer and UBSan
#   make clean
#
# FatFs resolves #include "ffconf.h" relative to the directory ff.h lives in,
# so no -I order can swap in a host configuration. The build directory
# therefore holds copies of ff.c/ff.h/diskio.h/ffunicode.c next to a copy of
# ffconf_host.h named ffconf.h (firmware config + write support and f_mkfs).
# sw/include/pg_debug.h pulls in two pico-sdk headers; stubs/ stands in for
# them.

CC      ?= cc
B       := build
CDROM   := ..
SW      := ../..
FATFS   := $(SW)/fatfs/source
CFLAGS  ?= -O1 -g
WARN    := -Wall -Wextra -Wno-unused-parameter
INC     := -I$(B) -Istubs -I$(CDROM) -I$(SW) -I$(SW)/../common
ifdef SAN
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
endif

LINKS := $(B)/ffconf.h $(B)/ff.c $(B)/ff.h $(B)/diskio.h $(B)/ffunicode.c
OBJS  := $(B)/cdrom_image_manager.o $(B)/cdrom_path.o $(B)/cdrom_image_backend.o \
         $(B)/cdrom_error_msg.o $(B)/ff.o $(B)/ffunicode.o \
         $(B)/ramdisk_diskio.o $(B)/test_cdrom_images.o
HDRS  := $(CDROM)/cdrom.h $(CDROM)/cdrom_image_manager.h $(CDROM)/cdrom_path.h \
         $(CDROM)/cdrom_image_backend.h $(CDROM)/cdrom_error_msg.h \
         $(SW)/../common/picogus.h $(SW)/include/pg_debug.h ramdisk_diskio.h \
         stubs/hardware/uart.h stubs/hardware/gpio.h

.PHONY: all test clean

all: $(B)/test_cdrom_images

test: $(B)/test_cdrom_images
	./$(B)/test_cdrom_images

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

$(B)/%.o: $(CDROM)/%.c $(LINKS) $(HDRS)
	$(CC) $(CFLAGS) $(WARN) $(INC) -c $< -o $@

$(B)/%.o: %.c $(LINKS) $(HDRS)
	$(CC) $(CFLAGS) $(WARN) $(INC) -c $< -o $@

$(B)/test_cdrom_images: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

clean:
	rm -rf $(B)
