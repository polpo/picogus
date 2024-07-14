//
// Copyright(C) 2005-2014 Simon Howard
// Copyright(C) 2021-2022 Graham Sanderson
// Copyright(C) 2024 Ian Scott
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
//     OPL SDL interface.
//

// todo replace opl_queue with pheap
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "emu8950.h"

#include "opl.h"
#include "hardware/sync.h"
#include "hardware/timer.h"

#define MAX_SOUND_SLICE_TIME 100 /* ms */

typedef struct
{
    unsigned int rate;        // Number of times the timer is advanced per sec.
    unsigned int enabled;     // Non-zero if timer is enabled.
    unsigned int value;       // Last value that was set.
    uint64_t expire_time;     // Calculated time that timer will expire.
} opl_timer_t;


typedef struct
{
    opl_timer_t timer1;       // Timer 1
    opl_timer_t timer2;       // Timer 2
    int register_num;         // Selected register number
    OPL *emu8950_opl;         // Instance of emulated OPL
} opl_instance_t;

static opl_instance_t opl_l = {
    .timer1 = { 12500, 0, 0, 0 },
    .timer2 = { 3125, 0, 0, 0 },
    .register_num = 0,
    .emu8950_opl = 0
};
static opl_instance_t opl_r = {
    .timer1 = { 12500, 0, 0, 0 },
    .timer2 = { 3125, 0, 0, 0 },
    .register_num = 0,
    .emu8950_opl = 0
};

void OPL_Pico_simple(int16_t *buffer, uint32_t nsamples) {
    OPL_calc_buffer(opl_l.emu8950_opl, buffer, nsamples);
}

void OPL_Pico_stereo(int16_t *buffer_l, int16_t *buffer_r, uint32_t nsamples) {
    OPL_calc_buffer(opl_l.emu8950_opl, buffer_l, nsamples);
    OPL_calc_buffer(opl_r.emu8950_opl, buffer_r, nsamples);
}

int OPL_Pico_Init(void)
{
    opl_l.emu8950_opl = OPL_new(3579552, PICO_SOUND_SAMPLE_FREQ); // todo check rate
    opl_r.emu8950_opl = OPL_new(3579552, PICO_SOUND_SAMPLE_FREQ); // todo check rate
    return 1;
}


unsigned int OPL_Pico_PortRead(uint16_t port)
{
    // OPL2 has 0x06 in its status register. If this is 0, it'll get detected as an OPL3...
    unsigned int result = 0x06;

    // port+0 - left OPL2
    // port+2 - right OPL2
    // port+8 - both OPL2s (write to both, read from left)
    opl_instance_t* opl;
    if (port & 8) {
        opl = &opl_l;
    } else if (port & 2) {
        opl = &opl_r;
    } else {
        opl = &opl_l;
    }

    __dsb();
    // Use time_us_64 as current_time gets updated coarsely as the mix callback is called
    uint64_t pico_time = time_us_64();
    if (opl->timer1.enabled && pico_time > opl->timer1.expire_time)
    {
        result |= 0x80;   // Either have expired
        result |= 0x40;   // Timer 1 has expired
    }

    if (opl->timer2.enabled && pico_time > opl->timer2.expire_time)
    {
        result |= 0x80;   // Either have expired
        result |= 0x20;   // Timer 2 has expired
    }

    return result;
}

static void OPLTimer_CalculateEndTime(opl_timer_t *timer)
{
    int tics;

    // If the timer is enabled, calculate the time when the timer
    // will expire.

    if (timer->enabled)
    {
        tics = 0x100 - timer->value;

        timer->expire_time = time_us_64()
                           + ((uint64_t) tics * OPL_SECOND) / timer->rate;
    }
}

static void WriteRegister(opl_instance_t* opl, unsigned int reg_num, unsigned int value)
{
    switch (reg_num)
    {
#if !EMU8950_NO_TIMER
        case OPL_REG_TIMER1:
            opl->timer1.value = value;
            OPLTimer_CalculateEndTime(&opl->timer1);
            //printf("timer1 set");
            break;

        case OPL_REG_TIMER2:
            opl->timer2.value = value;
            OPLTimer_CalculateEndTime(&opl->timer2);
            break;

        case OPL_REG_TIMER_CTRL:
            if (value & 0x80)
            {
                opl->timer1.enabled = 0;
                opl->timer2.enabled = 0;
            }
            else
            {
                if ((value & 0x40) == 0)
                {
                    opl->timer1.enabled = (value & 0x01) != 0;
                    OPLTimer_CalculateEndTime(&opl->timer1);
                }

                if ((value & 0x20) == 0)
                {
                    opl->timer1.enabled = (value & 0x02) != 0;
                    OPLTimer_CalculateEndTime(&opl->timer2);
                }
            }

            break;
#endif
        case OPL_REG_NEW:
        default:
            OPL_writeReg(opl->emu8950_opl, reg_num, value);
            break;
    }
}

void OPL_Pico_PortWrite(uint16_t port, unsigned int value)
{
    // port+0 - left OPL2
    // port+2 - right OPL2
    // port+8 - both OPL2s (write to both, read from left)
    if ((port & 1) == OPL_REGISTER_PORT)
    {
        if (port & 8) {
            opl_l.register_num = value;
            opl_r.register_num = value;
        } else if (port & 2) {
            opl_r.register_num = value;
        } else {
            opl_l.register_num = value;
        }
    }
    else if ((port & 1) == OPL_DATA_PORT)
    {
        if (port & 8) {
            WriteRegister(&opl_l, opl_l.register_num, value);
            WriteRegister(&opl_r, opl_r.register_num, value);
        } else if (port & 2) {
            WriteRegister(&opl_r, opl_r.register_num, value);
        } else {
            WriteRegister(&opl_l, opl_l.register_num, value);
        }
    }
}
