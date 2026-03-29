#pragma once

#include "include/dosbox-x-compat.h"

extern void GUS_OnReset(Bitu base_port);
extern Bitu read_gus(Bitu port);
extern void write_gus(Bitu port, Bitu val);
extern uint32_t GUS_CallBack(Bitu len, int16_t* play_buffer);
extern uint32_t GUS_sample_stereo(void);
extern uint8_t GUS_activeChannels(void);
extern uint32_t GUS_basefreq(void);
extern void GUS_Setup(void);
extern void GUS_SetFixed44k(const bool new_force44k);
extern void GUS_SetAudioBuffer(const uint16_t new_buffer_size);
