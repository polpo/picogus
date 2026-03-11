/*
 * opl_ymfm.cpp — OPL_Pico_* API implementation using ymfm
 *
 * Chip selection (compile-time, via CMake define):
 *   USE_YMF3812  — ymfm::ym3812  (OPL2): 9 ch / 18 op / 1 output
 *                  Status 0x06 → games detect OPL2. Mono output.
 *   (default)   — ymfm::ymf262  (OPL3): 18 ch / 36 op / 4 outputs
 *                  Status 0x00 → games detect OPL3. True stereo output.
 *
 * ymf262 output layout (data[0..3]):
 *   data[0] = L1, data[1] = R1, data[2] = L2, data[3] = R2
 *   Per-channel panning (reg 0xC0 bits 4-7) routes each channel to any
 *   combination of the four outputs. Stereo: L = data[0]+data[2],
 *   R = data[1]+data[3]. Mono: sum all four, shift right 2.
 *
 * Port read behavior:
 *   Bank 1 (port+0) and Bank 2 (port+2) both return the status register.
 *   OPL3 detection: port+2 returns valid status (not 0xFF), and status
 *   base is 0x00 (vs 0x06 for OPL2).
 */

#include "opl.h"
#include "hardware/sync.h"
#include "hardware/timer.h"

#include "ymfm/src/ymfm_opl.h"

#if OPL_CMD_BUFFER
#include "include/cmd_buffers.h"
extern cms_buffer_t opl_cmd_buffer;
#endif

// ---------------------------------------------------------------------------
// Chip selection
// ---------------------------------------------------------------------------
#ifdef USE_YMF3812
using opl_chip_t = ymfm::ym3812;
static constexpr unsigned int OPL_STATUS_BASE = 0x06; // OPL2 detection
#else
using opl_chip_t = ymfm::ymf262;
static constexpr unsigned int OPL_STATUS_BASE = 0x00; // OPL3 detection
#endif

// ---------------------------------------------------------------------------
// ymfm interface — minimal, all no-ops. Timers are managed by us, not ymfm.
// ---------------------------------------------------------------------------
class pico_ymfm_interface : public ymfm::ymfm_interface {};

static pico_ymfm_interface s_intf;
static opl_chip_t s_chip(s_intf);

// ---------------------------------------------------------------------------
// Timer state — Core 0 only
// ---------------------------------------------------------------------------
typedef struct
{
    unsigned int rate;
    unsigned int enabled;
    unsigned int value;
    uint64_t expire_time;
} opl_timer_t;

static opl_timer_t timer1 = { 12500, 0, 0, 0 };
static opl_timer_t timer2 = { 3125,  0, 0, 0 };

static void OPLTimer_CalculateEndTime(opl_timer_t *timer)
{
    if (timer->enabled)
    {
        int tics = 0x100 - timer->value;
        timer->expire_time = time_us_64()
                           + ((uint64_t)tics * OPL_SECOND) / timer->rate;
    }
}

// ---------------------------------------------------------------------------
// Pre-generation buffer — batch PREBUF_SIZE samples per generate() call so
// ymfm's inner operator loop runs with code and ROM tables hot in cache.
// Stereo L/R buffers used by both OPL_Pico_simple and OPL_Pico_stereo.
// ---------------------------------------------------------------------------
static constexpr uint32_t PREBUF_SIZE = 128;
static opl_chip_t::output_data s_gen_buf;
static int32_t s_prebuf_l[PREBUF_SIZE];
static int32_t s_prebuf_r[PREBUF_SIZE];
static uint32_t s_prebuf_head = PREBUF_SIZE; // starts empty → triggers fill on first call

static void refill_prebuf()
{
    for (uint32_t j = 0; j < PREBUF_SIZE; j++)
    {
#if OPL_CMD_BUFFER
        // Process any pending OPL commands
        while (opl_cmd_buffer.tail != opl_cmd_buffer.head) {
            auto cmd = opl_cmd_buffer.cmds[opl_cmd_buffer.tail];
            OPL_Pico_WriteRegister(cmd.addr, cmd.data);
            ++opl_cmd_buffer.tail;
            if ((cmd.addr < 0x20) || ((cmd.addr & 0xF0) == 0xB0)) break; // break on key on/off retrigs and misc registers
            break;
        }
#endif

        s_chip.generate(&s_gen_buf, 1);
#ifdef USE_YMF3812
        // ym3812 is mono — duplicate to both channels
        s_prebuf_l[j] = s_prebuf_r[j] = s_gen_buf.data[0];
#else
        // ymf262: use L1 (data[0]) and R1 (data[1]) — the standard stereo pair.
        // data[2]/data[3] are the rarely-used L2/R2 extended outputs, ignored.
        s_prebuf_l[j] = s_gen_buf.data[0];
        s_prebuf_r[j] = s_gen_buf.data[1];
#endif
    }
    s_prebuf_head = 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int OPL_Pico_Init(unsigned int port_base)
{
    s_chip.reset();
    s_prebuf_head = PREBUF_SIZE;
    return 1;
}

unsigned int OPL_Pico_PortRead(opl_port_t port)
{
    unsigned int result = OPL_STATUS_BASE;
    __dmb();
    uint64_t pico_time = time_us_64();

    if (timer1.enabled && pico_time > timer1.expire_time)
    {
        result |= 0x80 | 0x40;
    }
    if (timer2.enabled && pico_time > timer2.expire_time)
    {
        result |= 0x80 | 0x20;
    }

    return result;
}

void OPL_Pico_WriteRegister(unsigned int reg_num, unsigned int value)
{
    switch (reg_num)
    {
        case OPL_REG_TIMER1:
            timer1.value = value;
            OPLTimer_CalculateEndTime(&timer1);
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
                    timer2.enabled = (value & 0x02) != 0;
                    OPLTimer_CalculateEndTime(&timer2);
                }
            }
            break;

        default:
#ifndef USE_YMF3812
            if (reg_num >= 0x100)
                s_chip.write_address_hi(reg_num & 0xFF);
            else
                s_chip.write_address(reg_num);
#else
            s_chip.write_address(reg_num & 0xFF);
#endif
            s_chip.write_data(value & 0xFF);
            break;
    }
}

// Mono interface — kept for compatibility with non-ymfm OPL backends.
// Returns the average of L+R so mono output is at the same level as stereo.
void OPL_Pico_simple(int32_t *buffer, uint32_t nsamples)
{
    for (uint32_t i = 0; i < nsamples; i++)
    {
        if (s_prebuf_head >= PREBUF_SIZE)
            refill_prebuf();
        buffer[i] = (s_prebuf_l[s_prebuf_head] + s_prebuf_r[s_prebuf_head]) >> 1;
        s_prebuf_head++;
    }
}

// Stereo interface — fills separate left and right buffers.
// For ym3812 both channels are identical (mono chip).
// For ymf262 L and R reflect the per-channel panning programmed by the game.
void OPL_Pico_stereo(int32_t *left, int32_t *right, uint32_t nsamples)
{
    for (uint32_t i = 0; i < nsamples; i++)
    {
        if (s_prebuf_head >= PREBUF_SIZE)
            refill_prebuf();
        left[i]  = s_prebuf_l[s_prebuf_head];
        right[i] = s_prebuf_r[s_prebuf_head];
        s_prebuf_head++;
    }
}
