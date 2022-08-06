#pragma once

#include "dosbox-x-compat.h"

extern void GUS_OnReset();
extern Bitu read_gus(Bitu port,Bitu iolen);
extern void write_gus(Bitu port,Bitu val,Bitu iolen);
extern void GUS_CallBack(Bitu len, int16_t* play_buffer);
extern uint8_t GUS_activeChannels(void);
extern uint32_t GUS_basefreq(void);
