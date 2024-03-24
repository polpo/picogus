#ifdef INTERP_CLAMP
#include "hardware/interp.h"
#endif

static void clamp_setup(void) {
#ifdef INTERP_CLAMP
    interp_config cfg;
    // Clamp setup
    cfg = interp_default_config();
    interp_config_set_clamp(&cfg, true);
    interp_config_set_shift(&cfg, 14);
    // set mask according to new position of sign bit..
    interp_config_set_mask(&cfg, 0, 17);
    // ...so that the shifted value is correctly sign extended
    interp_config_set_signed(&cfg, true);
    interp_set_config(interp1, 0, &cfg);
    interp1->base[0] = -32768;
    interp1->base[1] = 32767;
#endif
}


static int16_t __force_inline clamp16(int32_t d) {
#ifdef INTERP_CLAMP
    interp1->accum[0] = d;
    return interp1->peek[0];
#else
    return d < -32768 ? -32768 : (d > 32767 ? 32767 : d);
#endif // INTERP_CLAMP
}
