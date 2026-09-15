; Part of PGDFS (PicoGUS DOS file system), a modified copy of the EtherDFS
; v0.8.2 client by Mateusz Viste (MIT license, see LICENSE). This file is
; unchanged apart from this note.

; this is a copy of the _chain_intr() function, borrowed as-is from the
; source code of OpenWatcom 1.9 (bld/clib/intel/a/chint086.asm). I only
; changed the name of the procedure to _mvchain_intr() to avoid confusion.
; the reason I'm doing this is to have it inside my own code segment, so I
; can use it from within the TSR after dropping all libc */


;*****************************************************************************
;*
;*                            Open Watcom Project
;*
;*    Portions Copyright (c) 1983-2002 Sybase, Inc. All Rights Reserved.
;*
;*  ========================================================================
;*
;*    This file contains Original Code and/or Modifications of Original
;*    Code as defined in and that are subject to the Sybase Open Watcom
;*    Public License version 1.0 (the 'License'). You may not use this file
;*    except in compliance with the License. BY USING THIS FILE YOU AGREE TO
;*    ALL TERMS AND CONDITIONS OF THE LICENSE. A copy of the License is
;*    provided with the Original Code and Modifications, and is also
;*    available at www.sybase.com/developer/opensource.
;*
;*    The Original Code and all software distributed under the License are
;*    distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
;*    EXPRESS OR IMPLIED, AND SYBASE AND ALL CONTRIBUTORS HEREBY DISCLAIM
;*    ALL SUCH WARRANTIES, INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF
;*    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR
;*    NON-INFRINGEMENT. Please see the License for the specific language
;*    governing rights and limitations under the License.
;*
;*  ========================================================================
;*
;* Description:  16-bit chain interrupt handler function.
;*
;*****************************************************************************


;include mdef.inc
;include struct.inc

        name    chainint

BEGTEXT   segment word public 'CODE'
BEGTEXT   ends


BEGTEXT   segment

_mvchain_intr     proc far
        public  "C",_mvchain_intr
        mov     cx,ax                   ; get offset
        mov     ax,dx                   ; get segment
        mov     sp,bp                   ; reset SP to point to saved registers
        xchg    cx,20[bp]               ; restore cx, & put in offset
        xchg    ax,22[bp]               ; restore ax, & put in segment
        mov     bx,28[bp]               ; restore flags
        and     bx,0FCFFh               ; except for IF and TF
        push    bx                      ; :
        popf                            ; :
        pop     dx                      ; pop off dummy fs entry
        pop     dx                      ; pop off dummy gs entry
        pop     es                      ; restore segment registers
        pop     ds                      ;
        pop     di                      ; . . .
        pop     si                      ; . . .
        pop     bp                      ; . . .
        pop     bx                      ; restore registers
        pop     bx                      ; . . .
        pop     dx                      ; . . .
        ret                             ; return to previous interrupt handler
_mvchain_intr     endp

BEGTEXT   ends
        end
