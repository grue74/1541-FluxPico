/**********************************
 * flux.h - flux stream into the drive's own read circuit (FLUX_MODE)
 *
 * The Pico plays the raw track as pulses on GPIO3, wired to the gate
 * array's CLR input (40-pin 251828: pin 31). The gate array decodes them
 * like read head pulses: SYNC, BYTE READY and port A come from the drive.
 * See doc/flux-mod-1541-II.pdf and doc/bitstream-plan.md.
 ***********************************/

#ifndef _FLUX_H_
#define _FLUX_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef FLUX_MODE

#include "pico/types.h"

void flux_init(void);                   // PIO, DMA and pins (core0, before the interrupts)
bool flux_calibrate(void);              // find pulse polarity and width; false: no response
void flux_service(void);                // main loop on core0: track changes, captured bytes
void flux_debug_service(void);          // prints flux events on USB serial
bool flux_gpio_irq(uint gpio, uint32_t events);   // GPIO interrupt hook, true: handled

void flux_image_stop(void);             // an image is being loaded: stop the stream
void flux_image_start(void);            // a new image is in memory: start at the current angle

extern volatile bool flux_ok;           // calibration found a working pulse shape

#endif

#endif
