# Makefile for PGDFS (GNU make + OpenWatcom 2.0), in the style of
# pgusinit/Makefile. The CI runs: make INCLUDE="$WATCOM"/h
#
# PGUSDFS.EXE is a TSR: it is built in the small model for the 8086, without
# stack checks (the resident part must not call libc), with a map file so the
# BEGTEXT (resident code) and DGROUP (resident data) sizes can be checked
# against DATASEGSZ in globals.h. DFSDIAG.EXE is a plain DOS program.
#
# -0      generate code for 8086 (the TSR must run on an 8088)
# -s      disable stack overflow checks (they would call libc from the TSR)
# -d0     no debugging information
# -ms     small memory model
# -os     optimize for size
# -wx -we all warnings, treated as errors (PICOGUS_NO_MODENAMES drops the
#         pgusinit-only modenames[] table from common/picogus.h)
# -k1024  1 KB stack for the transient part of the TSR
# -fm=    generate a map file

CC = wcl
AS = wasm
CFLAGS = -bcl=dos -0 -s -d0 -ms -os -wx -we -dPICOGUS_NO_MODENAMES
TSRFLAGS = $(CFLAGS) -k1024 -fm=pgusdfs.map

all: pgusdfs.exe dfsdiag.exe

chint.obj: chint086.asm
	$(AS) -0 chint086.asm -fo=chint.obj -ms

pgusdfs.exe: pgusdfs.c xport.c xport.h globals.h dosstruc.h chint.h version.h chint.obj ../common/picogus.h
	$(CC) $(TSRFLAGS) chint.obj pgusdfs.c -fe=pgusdfs.exe

dfsdiag.exe: dfsdiag.c xport.c xport.h version.h ../common/picogus.h
	$(CC) $(CFLAGS) -za99 dfsdiag.c xport.c -fe=dfsdiag.exe

clean:
	rm -f *.obj *.o *.err pgusdfs.exe dfsdiag.exe pgusdfs.map
