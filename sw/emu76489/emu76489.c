/****************************************************************************

  emu76489.c -- SN76489 emulator by Mitsutaka Okazaki 2001-2016

  2001 08-13 : Version 1.00
  2001 10-03 : Version 1.01 -- Added SNG_set_quality().
  2004 05-23 : Version 1.10 -- Implemented GG stereo mode by RuRuRu
  2004 06-07 : Version 1.20 -- Improved the noise emulation.
  2015 12-13 : Version 1.21 -- Changed own integer types to C99 stdint.h types.
  2016 09-06 : Version 1.22 -- Support per-channel output.

  References: 
    SN76489 data sheet   
    sn76489.c   -- from MAME
    sn76489.txt -- from http://www.smspower.org/

*****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emu76489.h"

static uint32_t voltbl[16] = {
  0xff, 0xcb, 0xa1, 0x80, 0x65, 0x50, 0x40, 0x33, 0x28, 0x20, 0x19, 0x14, 0x10, 0x0c, 0x0a, 0x00
};

#define GETA_BITS 24

static void
internal_refresh (SNG * sng)
{
  if (sng->quality)
  {
    sng->base_incr = 1 << GETA_BITS;
    sng->realstep = (uint32_t) ((1 << 31) / sng->rate);
    sng->sngstep = (uint32_t) ((1 << 31) / (sng->clk / 16));
    sng->sngtime = 0;
  }
  else
  {
    sng->base_incr = (uint32_t) ((double) sng->clk * (1 << GETA_BITS) / (16 * sng->rate));
  }
}

void
SNG_set_rate (SNG * sng, uint32_t r)
{
  sng->rate = r ? r : 44100;
  internal_refresh (sng);
}

void
SNG_set_quality (SNG * sng, uint32_t q)
{
  sng->quality = q;
  internal_refresh (sng);
}

SNG *
SNG_new (uint32_t c, uint32_t r)
{
  SNG *sng;

  sng = (SNG *) malloc (sizeof (SNG));
  if (sng == NULL)
    return NULL;

  sng->clk = c;
  sng->rate = r ? r : 44100;
  SNG_set_quality (sng, 0);

  return sng;
}

void
SNG_reset (SNG * sng)
{
  int i;

  sng->base_count = 0;

  for (i = 0; i < 3; i++)
  {
    sng->count[i] = 0;
    sng->freq[i] = 0;
    sng->edge[i] = 0;
    sng->volume[i] = 0x0f;
    sng->mute[i] = 0;
  }

  sng->adr = 0;

  sng->noise_seed = 0x8000;
  sng->noise_count = 0;
  sng->noise_freq = 0;
  sng->noise_volume = 0x0f;
  sng->noise_mode = 0;
  sng->noise_fref = 0;

  sng->out = 0;
  sng->stereo = 0xFF;

  sng->ch_out[0] = sng->ch_out[1] = sng->ch_out[2] = sng->ch_out[3] = 0;
}

void
SNG_delete (SNG * sng)
{
  free (sng);
}

void
SNG_writeIO (SNG * sng, uint32_t val)
{
  if (val & 0x80)
  {
    sng->adr = (val & 0x70) >> 4;
    switch (sng->adr)
    {
    case 0:
    case 2:
    case 4:
      sng->freq[sng->adr >> 1] = (sng->freq[sng->adr >> 1] & 0x3F0) | (val & 0x0F);
      break;

    case 1:
    case 3:
    case 5:
      sng->volume[(sng->adr - 1) >> 1] = val & 0xF;
      break;

    case 6:
      sng->noise_mode = (val & 4) >> 2;

      if ((val & 0x03) == 0x03) {
        sng->noise_freq = sng->freq[2];
        sng->noise_fref = 1;
      } else {
        sng->noise_freq = 32 << (val & 0x03);
        sng->noise_fref = 0;
      }

      if (sng->noise_freq == 0)
        sng->noise_freq = 1;

      sng->noise_seed = 0x8000;
      break;

    case 7:
      sng->noise_volume = val & 0x0f;
      break;
    }
  }
  else
  {
    sng->freq[sng->adr >> 1] = ((val & 0x3F) << 4) | (sng->freq[sng->adr >> 1] & 0x0F);
  }
}

static inline int parity(int val) {
	val^=val>>8;
	val^=val>>4;
	val^=val>>2;
	val^=val>>1;
	return val&1;
};

static inline void
update_output (SNG * sng)
{

  int i;
  uint32_t incr;

  sng->base_count += sng->base_incr;
  incr = (sng->base_count >> GETA_BITS);
  sng->base_count &= (1 << GETA_BITS) - 1;

  /* Noise */
  sng->noise_count += incr;
  if (sng->noise_count & 0x100)
  {
    if (sng->noise_mode) /* White */
      sng->noise_seed = (sng->noise_seed>>1) | (parity(sng->noise_seed&0x0009)<<15);
    else                 /* Periodic */
      sng->noise_seed = (sng->noise_seed>>1) | ((sng->noise_seed&1)<<15);
    
    if(sng->noise_fref)
      sng->noise_count -= sng->freq[2];
    else
      sng->noise_count -= sng->noise_freq;
  }

  if (sng->noise_seed & 1) 
  {
    sng->ch_out[3] += voltbl[sng->noise_volume] << 4;
  }
  sng->ch_out[3] >>= 1;

  /* Tone */
  for (i = 0; i < 3; i++)
  {
    sng->count[i] += incr;
    if (sng->count[i] & 0x400)
    {
      if (sng->freq[i] > 1)
      {
        sng->edge[i] = !sng->edge[i];
        sng->count[i] -= sng->freq[i];
      }
      else
      {
        sng->edge[i] = 1;
      }
    }

    if (sng->edge[i] && !sng->mute[i])
    {
      sng->ch_out[i] += voltbl[sng->volume[i]] << 4;
    }

    sng->ch_out[i] >>= 1;
  }

}

static inline int16_t
mix_output (SNG * sng) 
{
  sng->out = sng->ch_out[0] + sng->ch_out[1] + sng->ch_out[2] + sng->ch_out[3];
  return (int16_t) sng->out;
}

int16_t
SNG_calc (SNG * sng)
{
  if (!sng->quality) {
    update_output(sng);
    return mix_output(sng);
  }

  /* Simple rate converter */
  while (sng->realstep > sng->sngtime)
  {
    sng->sngtime += sng->sngstep;
    update_output(sng);
  }

  sng->sngtime = sng->sngtime - sng->realstep;

  return mix_output(sng);
}

static inline void 
mix_output_stereo (SNG * sng, int32_t out[2])
{
  int i;

  out[0] = out[1] = 0;
  if((sng->stereo>>4)&0x08) {
    out[0] += sng->ch_out[3];
  }
  if(sng->stereo & 0x08) {
    out[1] += sng->ch_out[3];
  }

  for (i = 0; i < 3; i++)
  {
    if ((sng->stereo >> (i+4)) & 0x01) {
      out[0] += sng->ch_out[i];
    }
    if ((sng->stereo >> i) & 0x01) {
      out[1] += sng->ch_out[i];
    }
  }

}

void
SNG_calc_stereo (SNG *sng, int32_t out[2])
{
  if (!sng->quality) {
    update_output(sng);
    mix_output_stereo(sng,out);
    return;
  }

  while (sng->realstep > sng->sngtime)
  {
    sng->sngtime += sng->sngstep;
    update_output(sng);
  }

  sng->sngtime = sng->sngtime - sng->realstep;
  mix_output_stereo(sng,out);
  return;
}

void
SNG_writeGGIO(SNG *sng, uint32_t val)
{
  sng->stereo = val;
}
