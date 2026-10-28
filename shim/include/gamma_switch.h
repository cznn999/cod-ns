/* The game's gamma ramp applied to the finished frame on the Switch
 * (shim/src/gamma_switch.c). */
#ifndef COD_GAMMA_SWITCH_H
#define COD_GAMMA_SWITCH_H

#include <stdint.h>

/* SetDeviceGammaRamp: 3 x 256 WORDs (red, green, blue). Any thread. */
void gamma_ramp_set(const uint16_t ramp[768]);
/* Draws the back buffer back through the ramp (nothing while it is the identity).
 * On the GL thread, just before the buffers are swapped. */
void gamma_apply(void);

#endif
