//
// Copyright(C) 2005-2014 Simon Howard
// Copyright(C) 2021-2022 Graham Sanderson
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//     OPL interface.
//


#ifndef OPL_OPL_H
#define OPL_OPL_H

#include <inttypes.h>

typedef enum
{
    OPL_REGISTER_PORT = 0,
    OPL_DATA_PORT = 1,
    OPL_REGISTER_PORT_OPL3 = 2
} opl_port_t;

#define OPL_REG_TIMER1            0x02
#define OPL_REG_TIMER2            0x03
#define OPL_REG_TIMER_CTRL        0x04

// Times

#define OPL_SECOND ((uint64_t) 1000 * 1000)

#ifdef __cplusplus
extern "C" {
#endif


int OPL_Pico_Init(unsigned int);
unsigned int OPL_Pico_PortRead(opl_port_t);
void OPL_Pico_WriteRegister(unsigned int, unsigned int);
void OPL_Pico_simple(int16_t*, uint32_t);

#ifdef __cplusplus
} // extern "C"
#endif

#endif
