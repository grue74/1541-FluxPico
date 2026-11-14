/**********************************
 * header - drive routines and control functions
 *
 * Author: F00K42
 * Last change: 2026/10/24
 ***********************************/

#ifndef _DRIVE_H_
#define _DRIVE_H_

#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "hardware/timer.h"

#include "menu.h"
#include "lcd.h"
#include "display.h"
#include "gui_constants.h"
#include "pinout.h"
#include "globals.h"

extern void (*display_clear)(void);
extern void (*display_home)(void);
extern void (*display_setcursor)(const uint8_t column, const uint8_t row);
extern void (*display_string)(const char* char_array);
extern void (*display_print)(const char* char_array, const uint8_t array_offset, const uint8_t print_length);

extern MENU_STRUCT image_menu;

// drive state and control functions
int64_t steppertimer_callback(alarm_id_t id, void *user_data);
void start_stepper_timer(void);
void check_stepper_signals(void);
void send_disk_change(void);
void init_writeprot(void);
void unmount_image(void);
void show_sdcard_info_message(void);

void init_stepper(void);
void stepper_inc(void);
void stepper_dec(void);
void set_head_near_track(const uint8_t track);   // head to/next to a track, in line with the stepper phase
void init_motor(void);
void init_control_signals(void);
void init_soe_gatearray(void);
void init_bytetimer(void);
void start_bytetimer(uint8_t half_track);
void stop_bytetimer(void);
void init_sound(void);

int64_t disk_alarm_callback(alarm_id_t id, void *user_data);

// drive activity for the display (written by the disk emulation)
extern volatile uint8_t  disk_header_sector;    // sector of the last header under the head, 0xFF: none
extern volatile uint32_t disk_header_ms;        // time of that header in ms
extern volatile uint8_t  disk_write_sector;     // sector of the last write
extern volatile uint32_t disk_write_ms;         // time of the last written byte in ms
void service_track_resolver(void);
uint32_t mark_track_dirty(void);
void drive_debug_service(void);   // prints the event log (DEBUG_DRIVE_LOG builds)

#endif
