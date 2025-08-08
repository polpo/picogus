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
//     OPL SDL interface.
//

// todo replace opl_queue with pheap
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include "pico/mutex.h"
#include "pico/util/pheap.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"

#if USE_WOODY_OPL
#include "woody_opl.h"
#elif USE_EMU8950_OPL
#include "emu8950.h"
#else
#include "opl3.h"
#endif

#include "opl.h"
/* #include "opl_internal.h" */

/* #include "opl_queue.h" */
typedef struct
{
    unsigned int rate;        // Number of times the timer is advanced per sec.
    unsigned int enabled;     // Non-zero if timer is enabled.
    unsigned int value;       // Last value that was set.
    uint64_t expire_time;     // Calculated time that timer will expire.
} opl_timer_t;

#define opl_op3mode 0
static OPL *emu8950_opl;

static opl_timer_t timer1 = { 12500, 0, 0, 0 };
static opl_timer_t timer2 = { 3125, 0, 0, 0 };

void OPL_Pico_simple(int16_t *buffer, uint32_t nsamples) {
    OPL_calc_buffer(emu8950_opl, buffer, nsamples);
}

int OPL_Pico_Init(unsigned int port_base)
{
    emu8950_opl = OPL_new(3579552, PICO_SOUND_SAMPLE_FREQ); // todo check rate
    return 1;
}

unsigned int OPL_Pico_PortRead(opl_port_t port)
{
    // OPL2 has 0x06 in its status register. If this is 0, it'll get detected as an OPL3...
    unsigned int result = 0x06;

    if (port == OPL_REGISTER_PORT_OPL3)
    {
        return 0xff;
    }

/* #if !EMU8950_NO_TIMER */
    __dsb();
    // Use time_us_64 as current_time gets updated coarsely as the mix callback is called
    uint64_t pico_time = time_us_64();
    if (timer1.enabled && pico_time > timer1.expire_time)
    {
        result |= 0x80;   // Either have expired
        result |= 0x40;   // Timer 1 has expired
    }

    if (timer2.enabled && pico_time > timer2.expire_time)
    {
        result |= 0x80;   // Either have expired
        result |= 0x20;   // Timer 2 has expired
    }
/* #endif */

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

void OPL_Pico_WriteRegister(unsigned int reg_num, unsigned int value)
{
    switch (reg_num)
    {
        case OPL_REG_TIMER1:
            timer1.value = value;
            OPLTimer_CalculateEndTime(&timer1);
            //printf("timer1 set");
            break;

        case OPL_REG_TIMER2:
            timer2.value = value;
            OPLTimer_CalculateEndTime(&timer2);
            break;

        case OPL_REG_TIMER_CTRL:
            if (value & 0x80)
            {
                timer1.enabled = 0;
                timer2.enabled = 0;
            }
            else
            {
                if ((value & 0x40) == 0)
                {
                    timer1.enabled = (value & 0x01) != 0;
                    OPLTimer_CalculateEndTime(&timer1);
                }

                if ((value & 0x20) == 0)
                {
                    timer1.enabled = (value & 0x02) != 0;
                    OPLTimer_CalculateEndTime(&timer2);
                }
            }

            break;
        default:
            OPL_writeReg(emu8950_opl, reg_num, value);
            break;
    }
}
