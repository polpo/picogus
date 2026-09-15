/* Part of PGDFS (PicoGUS DOS file system), a modified copy of the EtherDFS
 * v0.8.2 client by Mateusz Viste (MIT license, see LICENSE). This file is
 * unchanged apart from this note. */

/* this is a copy of the _chain_intr() function, borrowed as-is from the
 * source code of OpenWatcom 1.9 (bld/clib/intel/a/chint086.asm)
 * the reason I'm doing this is to have it inside my own code segment, so I
 * can use it from within the TSR after dropping all libc */

/* original declaration:
_WCRTLINK extern void     _chain_intr( void
                                      (_WCINTERRUPT _DOSFAR *__handler)() );
*/

#ifndef chint_h_sentinel
#define chint_h_sentinel

_WCRTLINK extern void _mvchain_intr(void far *__handler);

#endif
