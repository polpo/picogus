/*
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

#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/timer.h"
#include "hardware/vreg.h"

// 370MHz PLL configuration:
// 12MHz XOSC / 2 (REFDIV) = 6MHz reference
// 6MHz * 185 (FBDIV) = 1110MHz VCO
// 1110MHz / 3 (POSTDIV1) / 1 (POSTDIV2) = 370MHz
//
// 370MHz is extremely closely evenly divisible by 44100Hz, making it ideal for
// audio sample rate generation. This frequency cannot be achieved with the
// default REFDIV of 1 since 370 is not evenly divisible by 12.
static inline void overclock_370mhz(void) {
    // Bump voltage for stability at 370MHz
    if (vreg_get_voltage() < VREG_VOLTAGE_1_25) {
        vreg_set_voltage(VREG_VOLTAGE_1_25);
        busy_wait_us_32(10000);  // 10ms for voltage to settle
    }

    // Switch clk_sys to USB PLL (48MHz) while reconfiguring PLL_SYS
    clock_configure_undivided(clk_sys,
                    CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
                    CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    USB_CLK_HZ);

    // Init PLL_SYS with REFDIV=2 for 370MHz
    pll_init(pll_sys, 2, 1110000000u, 3, 1);

    // Configure clk_ref to XOSC
    clock_configure_undivided(clk_ref,
                    CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC,
                    0,
                    XOSC_HZ);

    // Switch clk_sys to PLL_SYS at 370MHz
    clock_configure_undivided(clk_sys,
                    CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
                    CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                    370000000u);

    // Peripheral clock stays on USB PLL (48MHz)
    clock_configure_undivided(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                    USB_CLK_HZ);
}
