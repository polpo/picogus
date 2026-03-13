// DOSBox OPL core adaptation for PicoGUS
// mostly just copied from opl_ymfm.cpp

#include "opl.h"
#include "hardware/sync.h"
#include "hardware/timer.h"

#include "dbopl/dbopl.h"

#if OPL_CMD_BUFFER
#include "include/cmd_buffers.h"
extern cms_buffer_t opl_cmd_buffer;
#endif

// ---------------------------------------------------------------------------
// Chip selection
// ---------------------------------------------------------------------------
#ifdef USE_YMF3812
static constexpr unsigned int OPL_STATUS_BASE = 0x06; // OPL2 detection
#else
static constexpr unsigned int OPL_STATUS_BASE = 0x00; // OPL3 detection
#endif

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

static DBOPL::Chip dbopl3(true);

// ---------------------------------------------------------------------------
// Pre-generation buffer — batch PREBUF_SIZE samples per generate() call so
// ymfm's inner operator loop runs with code and ROM tables hot in cache.
// Stereo L/R buffers used by both OPL_Pico_simple and OPL_Pico_stereo.
// ---------------------------------------------------------------------------
static constexpr uint32_t PREBUF_SIZE   = 128;
static int32_t s_prebuf[2*PREBUF_SIZE];         // interleaved stereo
static uint32_t s_prebuf_head = PREBUF_SIZE;    // starts empty → triggers fill on first call

static void refill_prebuf()
{
    int samples_to_render = PREBUF_SIZE;
    int32_t *pb = s_prebuf;
    while (opl_cmd_buffer.tail != opl_cmd_buffer.head) {
        auto cmd = opl_cmd_buffer.cmds[opl_cmd_buffer.tail];
        OPL_Pico_WriteRegister(cmd.addr, cmd.data);
        ++opl_cmd_buffer.tail;
        if ((cmd.addr < 0x20) || ((cmd.addr & 0xF0) == 0xB0)) {
            // step 1 OPL clock, then continue draining OPL register queue
            dbopl3.GenerateBlock3(1, pb);
            pb += 2 * 1;
            samples_to_render--;
            if (samples_to_render == 0) break;
        }; 
    }

    // render the rest
    if (samples_to_render > 0) dbopl3.GenerateBlock3(samples_to_render, pb);

    // reset prebuf head
    s_prebuf_head = 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int OPL_Pico_Init(unsigned int port_base)
{
    dbopl3.Setup(49716);
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
            dbopl3.WriteReg(reg_num, value);
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
        buffer[i] = (s_prebuf[2*s_prebuf_head+0] + s_prebuf[2*s_prebuf_head+1]) >> 1;
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
        left[i]  = s_prebuf[2*s_prebuf_head+0];
        right[i] = s_prebuf[2*s_prebuf_head+1];
        s_prebuf_head++;
    }
}
