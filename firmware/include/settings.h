/**********************************
 * header - persistent settings in flash
 *
 * The settings are kept in the last 4KB sector of the flash. Flashing a
 * new firmware (UF2) does not touch this sector, so they survive updates.
 *
 * Writing the flash stops code execution from flash on both cores, so a
 * save is only requested by the GUI (core1) and done by the main loop on
 * core0 with flash_safe_execute(), which parks core1 in RAM meanwhile.
 *
 * Last change: 2026/11/12
 ***********************************/

#ifndef _SETTINGS_H_
#define _SETTINGS_H_

#include <stdint.h>
#include <stdbool.h>

// read the settings from flash and apply them (defaults if none are stored)
void settings_load(void);

// the settings changed: save them (called from the GUI on core1)
void settings_request_save(void);

// main loop on core0: do a requested save
void settings_service(void);

#endif
