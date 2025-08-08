#pragma once

#include "include/dosbox-x-compat.h"

extern void GUS_OnReset(Bitu base_port);
extern Bitu read_gus(Bitu port);
extern void write_gus(Bitu port, Bitu val);
extern uint32_t GUS_CallBack(Bitu len, int16_t* play_buffer);
extern uint8_t GUS_activeChannels(void);
extern uint32_t GUS_basefreq(void);
extern void GUS_Setup(void);
