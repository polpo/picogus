# PGDFS - the PicoGUS USB drive as a DOS drive letter

PGDFS is a small DOS TSR (about 10 KB resident) that maps the USB drive
plugged into a PicoGUS ISA card to a DOS drive letter, in the same way
PicoMEM's PMDFS does. DOS sees a network drive: `DIR`, `COPY`, `TYPE`,
`CD`, `MD`, `DEL`, `REN`, `ATTRIB`, running programs from it, and so on all
work as usual. The card serves the file system itself from FatFs, so DOS
never sees sectors and the drive format is the card's business.

The DOS side is derived from Mateusz Viste's EtherDFS client; the requests
and answers are the EtherDFS "EDF5" protocol, moved over the ISA bus instead
of Ethernet. The protocol is documented in `sw/dfs/PROTOCOL.md`.

## Requirements

* A PicoGUS with firmware that includes PGDFS (protocol version 5 or later).
  Older firmware makes PGDFS print "this PicoGUS firmware has no PGDFS
  support"; upgrade with `pgusinit /flash picogus.uf2`.
* PGDFS enabled on the card. It is enabled by default (data port 1D4h, see
  [Data port](#data-port)); `pgusinit` shows its state and `pgusinit /dfsport`
  changes it.
* A FAT12/FAT16/FAT32-formatted USB drive plugged into the PicoGUS.
* MS-DOS 5.0 or later (or a compatible DOS such as FreeDOS). DOS must have a
  free drive letter: raise `LASTDRIVE=` in `CONFIG.SYS` if PGDFS complains
  that it cannot map the letter.
* Any PC: PGDFS runs on an 8088 upwards (it detects the CPU and moves data
  with `REP INSW`/`REP OUTSW` on an 80186 or later, with `IN AX,DX`/`OUT
  DX,AX` loops on an 8086/8088).

## Usage

```
PGUSDFS X: [/Q] [/R]     map the PicoGUS USB drive to drive X:
PGUSDFS /U [/Q]          unload PGUSDFS from memory
PGUSDFS /T               push the DOS date and time to the card and exit
```

* `X:` - the drive letter to use (any unused letter up to `LASTDRIVE`).
* `/R` - map the drive read-only. Writes, creates, deletes, renames,
  `MD`/`RD` and attribute changes are refused with "access denied" without
  bothering the card.
* `/Q` - quiet: print nothing when loading or unloading succeeded.
* `/U` - unload the TSR. Works when PGDFS was the last program to hook
  INT 2Fh (unload TSRs in the reverse order of loading).
* `/T` - only send the DOS clock to the card, whether or not the TSR is
  loaded. PGDFS does this at install time as well; the card uses the clock
  for the timestamps of files created or modified from DOS. Run `PGUSDFS /T`
  again after changing the DOS date or time.
* `/?` - help.

At install time PGDFS checks for the card, reads the data port the card is
configured for, prints the mounted USB drive (label, file system, size), the
data port and the resident size, and maps the letter. There is no port to
type on the command line: the card's setting is the only place it lives. The
USB drive does not have to be present when PGDFS loads: without one, any
access to the drive letter fails with "drive not ready" until a drive is
plugged in.

Example `AUTOEXEC.BAT` line: `PGUSDFS E: /Q`.

`PGUSINIT.EXE` shows the state of PGDFS on a `PGDFS data port 1D4, USB
drive: ...` line of its normal output when the firmware has PGDFS (or `PGDFS
disabled` when it has been switched off with `pgusinit /dfsport 0`).

## Data port

Besides the PicoGUS control registers on 1D0h-1D2h, PGDFS moves the request
and answer bytes through a data window of two consecutive I/O ports. The
window's base is a card setting like the base ports of the emulated sound
cards: `pgusinit /dfsport x` sets it (hex; even; 100h-3FEh; not 1D0h-1D3h;
default 1D4h, so the window is 1D4h-1D5h), `pgusinit /save` makes it
persistent, and `pgusinit /dfsport 0` disables PGDFS altogether (the card
then stops decoding the window and PGDFS refuses to load). The driver reads
the setting from the card at install time and prints it in its banner (`data
port 1D4h`), so there is nothing to configure on the DOS side; move the
window only when another card needs 1D4h-1D5h. PGDFS reads the port once at
install: after changing it (or after `pgusinit /defaults`) unload and reload
the driver (`PGUSDFS /U`, then `PGUSDFS E:`). Keep the window clear of the ports
of the emulated devices that are active in your mode (220h Sound Blaster,
250h CD-ROM, 330h MPU-401, 388h AdLib, ...): those are decoded first and
pgusinit warns when the window overlaps one of them.

Both ports of the window feed the same byte stream, so the driver transfers
data a word at a time (`REP INSW`/`REP OUTSW`, or `IN AX,DX`/`OUT DX,AX`
loops on an 8086/8088): the motherboard splits each 16-bit access to this
8-bit card into two 8-bit bus cycles (the base port, then base+1) without
any CPU work, which roughly halves the CPU cost per byte compared to byte
transfers. Nothing needs configuring for this either; `DFSDIAG /INFO` shows
which instructions are in use and `DFSDIAG /ECHO` measures the resulting
throughput.

## Test tool

`DFSDIAG.EXE` talks to the card through the same transport as the TSR
without installing anything, and doubles as the protocol conformance check
for the firmware. Every failure is reported with the status byte, the DOS
result (AX) and the lengths involved.

```
DFSDIAG /INFO              card, protocol, data port, frame size, USB drive, free space,
                           and the card's disk/FatFs diagnostics
DFSDIAG /ECHO [n]          echo 64/512/4096-byte payloads n times, verify, KB/s
DFSDIAG /DIR [path]        list a directory (FINDFIRST/FINDNEXT)
DFSDIAG /LDIR [path]       list a directory with long file names (LONGNAME)
DFSDIAG /TYPE file         show a file (READ)
DFSDIAG /GET remote local  copy a file from the USB drive
DFSDIAG /PUT local remote  copy a file to the USB drive
DFSDIAG /MKDIR path        create a directory (MKDIR)
DFSDIAG /WRTEST path [n]   create a file, write n bytes (512), read them back,
                           delete it; every step shows its DOS result
DFSDIAG /TIME              push the DOS clock to the card
```

Remote paths are relative to the root of the USB drive (`\DIR\FILE.TXT`); a
drive letter prefix is ignored. `DFSDIAG /INFO` followed by `DFSDIAG /ECHO`
is the first thing to run on new firmware. `/LDIR` prints each entry as
`SHORT.EXT  size  date time  Long Name`: the short name is what DOS sees,
the long name is what the card returns for that entry (see below), printed
byte for byte in the card's code page.

When something fails, `DFSDIAG` fetches the card's diagnostics record (the
`DIAG` request in `sw/dfs/PROTOCOL.md`) and prints it after the error, so one
screen says which layer gave up: the USB transfer (a WRITE(10) refused by the
USB stack, answered with an error status by the drive, timed out (2 s for a read, 10 s for a write),
or cut short by the drive disappearing, with the sector, count and duration
of the last write), FatFs (the last failing call and its result, and how many
free clusters FatFs believes the volume has) or the server. `/WRTEST` is the
smallest write round trip (CREATE, WRITE with sync, READ, CLOSE, DELETE) and
the thing to run, with `/MKDIR`, when writes fail under the TSR: paste its
output together with the diagnostics block.

## Limitations

- `COPY` does not preserve timestamps: a file copied to the drive gets the time of the copy (from the DOS clock pushed at install or with `/T`). EtherDFS behaves the same; DOS sets the source time on the handle and expects the redirector to apply it at close, which PGDFS does not do yet.

* DOS sees 8.3 names only. Long file names on the USB drive appear as their
  short aliases (`LONGNA~1.EXT`); files created from DOS get plain 8.3
  names. See [Long file names and code pages](#long-file-names-and-code-pages).
* One drive: the first FAT volume of the USB drive is served as remote drive
  index 0. exFAT is not supported yet.
* DOS 3.x is not supported yet (PGDFS uses the DOS 4+ layout of the SDA and
  the CDS, like EtherDFS).
* Windows 9x (DOS box and Windows itself) is untested.
* No file locking across programs: LOCK/UNLOCK calls are accepted and
  ignored, as in EtherDFS.
* Free space and total size are reported capped just under 2 GB, which is
  what DOS can represent.
* Unplugging the USB drive while files are open on it invalidates those
  files; programs get "drive not ready" on the next access.

## Long file names and code pages

The DOS network redirector interface is 8.3-only: DOS hands the driver 8.3
names and expects 8.3 names back, whatever the DOS version. A long file name
on the USB drive therefore shows up in `DIR` as the short alias FatFs
generates for it (`LONGNA~1.EXT`), and a file created from DOS gets a plain
8.3 name with no long name attached. The long names are not lost: they are
there on the stick itself (plug it into any other system) and they can be
seen from DOS with `DFSDIAG /LDIR [path]`, which lists a directory with
each entry's long name next to its short one, using the PGDFS `LONGNAME`
subfunction (the card answers with the entry's long name, or its short name
when it has none; see `sw/dfs/PROTOCOL.md`). Windows 9x is untested; being
served through the DOS redirector interface, the drive would show short
names there as well.

Names cross the wire in the OEM code page of the card's FatFs, a firmware
build option: `FATFS_CODE_PAGE` (CMake, default 437 = US). Build the
firmware with 850, 865 or another FatFs code page when files created from
DOS carry national characters in their names, so that the stick shows those
names correctly on other systems and long names printed by `/LDIR` come out
right on a DOS running the same code page. The code page only affects file
names, never file contents.

## Troubleshooting

* `PicoGUS not detected` - nothing answered on port 1D0h. Check that the
  card is seated and that `PGUSINIT` finds it.
* `This PicoGUS firmware has no PGDFS support` - the firmware is older than
  the PGDFS protocol (protocol 5), or older than the configurable data port.
  Upgrade the firmware with `pgusinit /flash`.
* `PicoGUS firmware uses protocol N` - same as above, older protocol.
* `PGDFS is disabled on this PicoGUS` - the data port setting on the card
  is 0. `pgusinit /dfsport 1D4` (then `pgusinit /save` to keep it) turns
  PGDFS back on.
* `Drive not ready` / "not ready reading drive X:" when accessing the drive
  - no USB drive is mounted on the card (none inserted, unplugged, or the
  card is still mounting it), the drive is not FAT-formatted, or the card
  did not answer within 30 seconds. `PGUSINIT` and `DFSDIAG /INFO` show
  what the card sees.
* `Write fault` / `Read fault` (DOS errors 1Dh/1Eh) - the drive is mounted
  but a sector transfer to or from it failed. `DFSDIAG /WRTEST \X.TMP` and
  the diagnostics it prints show whether the USB command was refused, was
  answered with an error status, timed out, or whether FatFs rejected the
  operation. Run `DFSDIAG /INFO` right after the failure: the record keeps
  the last error until the next one.
* `Access denied` on `MD` or `COPY` to the drive, with nothing read-only in
  the picture - FatFs believes the volume has no free cluster. The card no
  longer trusts the free count stored on the stick by other systems and
  counts the FAT itself on the first free-space query, so this should not
  happen any more; `DFSDIAG /INFO` shows the count FatFs is working with.
* `Cannot map this drive letter (LASTDRIVE too low?)` - raise `LASTDRIVE=`
  in `CONFIG.SYS` or pick a lower letter.
* `This drive letter is already in use` - the letter belongs to a local
  disk, a SUBST or another network drive.
* `PGUSDFS is already loaded` - use `PGUSDFS /U` first to change the mapping.
* Wrong timestamps on files created from DOS: run `PGUSDFS /T` after setting
  the DOS clock (the card keeps DOS time + elapsed time).
* Emulators: DOSBox and DOSBox-X do not emulate a PicoGUS, so PGDFS reports
  "PicoGUS not detected" there. The undocumented `/N` option installs the
  TSR without looking for the card (for testing the TSR mechanics); it
  needs a real DOS kernel with a CDS, which DOSBox-X's built-in DOS does not
  provide.

## Performance notes

Every DOS file operation becomes one request/answer transaction over the
ISA bus: the request bytes are streamed to the data window (1D4h-1D5h by
default), the card serves it from FatFs on its second core, and the answer
is streamed back. The transfer runs at ISA I/O speed (roughly 1 microsecond
per byte on the bus; the word-wide `REP INSW`/`OUTSW` transfers keep the CPU
out of the way on a 286 or better, the 8088 loop is several times slower),
so throughput is bounded by the bus and by the USB drive on the card. Reads and writes are
chunked to the frame payload size (4096 bytes, or less if the firmware
reports a smaller `CMD_DFSMAXLEN`); DOS programs that read in large blocks
get the best speed, programs that read byte by byte pay one full transaction
per call. `DFSDIAG /ECHO` measures the raw transport speed on a given
machine.

The resident part keeps one 4100-byte frame buffer plus a private stack, so
it needs about 10 KB of conventional memory; it can be loaded high with
`LOADHIGH`/`LH`.

## Building

OpenWatcom 2.0: `make` (GNU make, as used by the CI) or `wmake -f makefile.wat`
builds `PGUSDFS.EXE` and `DFSDIAG.EXE`. The TSR is compiled with `-0 -s -ms`
(8086 code, no stack checks, small model); its resident code lives in the
`BEGTEXT` segment and must not call the C library. Check `pgusdfs.map` after
changes: the `DGROUP` size must not exceed `DATASEGSZ` in `globals.h`.

## Credits

* EtherDFS by Mateusz Viste (MIT license), whose DOS client PGDFS is
  derived from, and whose EDF5 protocol it speaks:
  http://etherdfs.sourceforge.net
* PMDFS in the PicoMEM project by FreddyV, the model for serving EtherDFS
  requests from a FatFs volume on an ISA card.
* `chint086.asm` contains code from the Open Watcom project (Sybase Open
  Watcom Public License).

See `LICENSE` in this directory.
