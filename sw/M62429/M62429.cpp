//
//    FILE: M62429.cpp
//  AUTHOR: Rob Tillaart
// PURPOSE: Arduino library for M62429 volume control IC
// VERSION: 0.3.6
// HISTORY: See M62429.cpp2
//     URL: https://github.com/RobTillaart/M62429

// HISTORY: see changelog.md

// Adapted by Ian Scott for Raspberry Pi Pico SDK

#include "M62429.h"

#include "hardware/gpio.h"
#include "hardware/timer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#ifdef M62429_PIO
#include "M62429.pio.h"
#endif

#define M62429_MAX_ATTN           87      //  decibel

// Translate arduino levels to bool
#define LOW false
#define HIGH true

void M62429::begin(uint8_t dataPin, uint8_t clockPin
#ifdef M62429_PIO
                   , PIO pio, int sm
#endif
                  )
{
#ifdef M62429_PIO
  _pio = pio;
  uint sm_offset = pio_add_program(_pio, &m62429_program);
  if (sm == -1) {
    _sm = pio_claim_unused_sm(_pio, true);
  } else {
    _sm = sm;
  }
  m62429_program_init(_pio, _sm, sm_offset, dataPin, clockPin);
#else
  _data = dataPin;
  _clock = clockPin;

  gpio_init(_data);
  gpio_init(_clock);
  gpio_set_dir(_data, GPIO_OUT);
  gpio_set_dir(_clock, GPIO_OUT);
  gpio_put(_data, LOW);
  gpio_put(_clock, LOW);
#endif //M62429_PIO

  _muted = false;
  setVolume(2, 0);
}


int M62429::getVolume(uint8_t channel)
{
  if (channel > 2) return M62429_CHANNEL_ERROR;
  return _vol[channel & 1];
}


int M62429::setVolume(uint8_t channel, uint8_t volume)
{
  if (channel > 2) return M62429_CHANNEL_ERROR;
  if (_muted)      return M62429_MUTED;

  // loudness to DB - volume is given on a scale from 0 - 100, 0 being silent and 100 full volume
  _setAttn(channel, std::max(0l, std::lround(87.0 + (33.22 * std::log10((float)std::min<uint8_t>(volume, 100u) / 100.0)))));

  //  update cached values
  if (channel == 0)      _vol[0] = volume;
  else if (channel == 1) _vol[1] = volume;
  else                   _vol[0] = _vol[1] = volume;
  return M62429_OK;
}


int M62429::incr(uint8_t channel)
{
  if (channel > 2) return M62429_CHANNEL_ERROR;
  if (_muted) return M62429_MUTED;

  if ( ((channel == 0) || (channel == 2))  && (_vol[0] < 100))
  {
    _vol[0]++;
    setVolume(0, _vol[0]);
  }
  if ( ((channel == 1) || (channel == 2)) && (_vol[1] < 100))
  {
    _vol[1]++;
    setVolume(1, _vol[1]);
  }
  return M62429_OK;
}


int M62429::decr(uint8_t channel)
{
  if (channel > 2) return M62429_CHANNEL_ERROR;
  if (_muted) return M62429_MUTED;

  if ( ((channel == 0) || (channel == 2)) && (_vol[0] > 0))
  {
    _vol[0]--;
    setVolume(0, _vol[0]);
  }
  if ( ((channel == 1) || (channel == 2)) && (_vol[1] > 0))
  {
    _vol[1]--;
    setVolume(1, _vol[1]);
  }
  return M62429_OK;
}


int M62429::average()
{
  if (_muted) return M62429_MUTED;
  uint8_t v = (((int)_vol[0]) + _vol[1]) / 2;
  setVolume(2, v);
  return M62429_OK;
}


void M62429::muteOn()
{
  if (_muted) return;
  _muted = true;
  //  if ((_vol[0] > 0) || (_vol[1] > 0)) _setAttn(2, 0);
  _setAttn(2, 0);     //  mute must work unconditional.
}


void M62429::muteOff()
{
  if (_muted == false) return;
  _muted = false;
  if (_vol[0] > 0) setVolume(0, _vol[0]);
  if (_vol[1] > 0) setVolume(1, _vol[1]);
}


////////////////////////////////////////////////////////////////////
//
//  PRIVATE
//

//  attn = 0..M62429_MAX_ATTN
void M62429::_setAttn(uint8_t channel, uint8_t attn)
{
  uint16_t databits = 0x0200;               //         D9 latch bit
  databits |= ((attn & 0x03) << 7);         //  D8  -  D7
  databits |= (attn & 0x7C);                //  D6  -  D2
  //  channel == 2 -> both 0x00 is default
  if (channel == 0) databits |= 0x03;       //  D0  -  D1
  if (channel == 1) databits |= 0x02;       //  D0  -  D1

#ifdef M62429_PIO
  pio_sm_put(_pio, _sm, databits);
#else // M62429_PIO
  // write D0 - D9
  for (uint8_t i = 0; i < 10; i++)
  {
    gpio_put(_data, databits & 0x01);
    databits >>= 1;
    gpio_put(_clock, HIGH);
    //  Note if _clock pulses are long enough, _data pulses are too.
    #if M62429_CLOCK_DELAY > 0
    busy_wait_us(M62429_CLOCK_DELAY);
    #endif

    gpio_put(_data, LOW);
    gpio_put(_clock, LOW);
    #if M62429_CLOCK_DELAY > 0
    busy_wait_us(M62429_CLOCK_DELAY);
    #endif
  }

  //  Send D10 HIGH bit (Latch signal)
  gpio_put(_data, HIGH);
  gpio_put(_clock, HIGH);
  #if M62429_CLOCK_DELAY > 0
  busy_wait_us(M62429_CLOCK_DELAY);
  #endif

  //  latch D10  signal requires _clock low before _data
  //  make _data dummy write to keep timing constant
  gpio_put(_data, HIGH);
  gpio_put(_clock, LOW);
  #if M62429_CLOCK_DELAY > 0
  busy_wait_us(M62429_CLOCK_DELAY);
  #endif

  gpio_put(_data, LOW);
  #if M62429_CLOCK_DELAY > 0
  busy_wait_us(M62429_CLOCK_DELAY);
  #endif
#endif // M62429_PIO
}


/////////////////////////////////////////////////////////////////////////////
//
//  M62429_RAW
//
void M62429_RAW::begin(uint8_t dataPin, uint8_t clockPin)
{
  _data = dataPin;
  _clock = clockPin;
  gpio_init(_data);
  gpio_init(_clock);
  gpio_set_dir(_data, GPIO_OUT);
  gpio_set_dir(_clock, GPIO_OUT);
  gpio_put(_data, LOW);
  gpio_put(_clock, LOW);

  setAttn(2, 0);
}


int M62429_RAW::getAttn(uint8_t channel)
{
  return _attn[channel & 1];
}


void M62429_RAW::setAttn(uint8_t channel, uint8_t attn)
{
  uint16_t databits = 0x0200;               //         D9 latch bit
  databits |= ((attn & 0x03) << 7);         //  D8  -  D7
  databits |= (attn & 0x7C);                //  D6  -  D2
  //  channel == 2 -> both 0x00 is default
  if (channel == 0) databits |= 0x03;       //  D0  -  D1
  if (channel == 1) databits |= 0x02;       //  D0  -  D1

  //  write D0 - D9
  for (uint8_t i = 0; i < 10; i++)
  {
    gpio_put(_data, databits & 0x01);
    databits >>= 1;
    gpio_put(_clock, HIGH);
    // Note if _clock pulses are long enough, _data pulses are too.
    #if M62429_CLOCK_DELAY > 0
    busy_wait_us(M62429_CLOCK_DELAY);
    #endif

    gpio_put(_data, LOW);
    gpio_put(_clock, LOW);
    #if M62429_CLOCK_DELAY > 0
    busy_wait_us(M62429_CLOCK_DELAY);
    #endif
  }

  //  Send D10 HIGH bit (Latch signal)
  gpio_put(_data, HIGH);
  gpio_put(_clock, HIGH);
  #if M62429_CLOCK_DELAY > 0
  busy_wait_us(M62429_CLOCK_DELAY);
  #endif

  //  latch D10  signal requires _clock low before _data
  //  make _data dummy write to keep timing constant
  gpio_put(_data, HIGH);
  gpio_put(_clock, LOW);
  #if M62429_CLOCK_DELAY > 0
  busy_wait_us(M62429_CLOCK_DELAY);
  #endif

  gpio_put(_data, LOW);
  #if M62429_CLOCK_DELAY > 0
  busy_wait_us(M62429_CLOCK_DELAY);
  #endif

  //  update cached values
  if (channel == 0)      _attn[0] = attn;
  else if (channel == 1) _attn[1] = attn;
  else                   _attn[0] = _attn[1] = attn;
}


// -- END OF FILE --

