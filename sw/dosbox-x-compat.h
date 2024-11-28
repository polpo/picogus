/*
 *  Copyright (C) 2002-2021  The DOSBox Team
 *  Copyright (C) 2022-2024  Ian Scott
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#pragma once

#include "pico/platform.h"

typedef uint32_t	Bitu;

//#define INLINE inline __attribute__((always_inline))
#define INLINE __force_inline

typedef uint8_t const *       ConstHostPt;        /* host (virtual) memory address aka ptr */
static INLINE uint16_t host_readw(ConstHostPt off) {
    return __builtin_bswap16(*(uint16_t *)off);
}

#ifdef DEBUG
#define LOG_MSG(msg, ...) printf(msg "\n", ##__VA_ARGS__);
#define DEBUG_LOG_MSG(msg, ...) printf(msg "\n", ##__VA_ARGS__);
#else
#define LOG_MSG(...) (void)0
#define DEBUG_LOG_MSG(...) (void)0
#endif
