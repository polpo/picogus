obj = pgusinit.obj
bin = pgusinit.exe

CC = wcc
CFLAGS = -za99
LD = wlink

$(bin): $(obj)
	$(LD) name $@ file { $(obj) }

.c.obj:
	$(CC) -fo=$@ $(CFLAGS) $<

clean: .symbolic
	del *.obj
	del $(bin)
