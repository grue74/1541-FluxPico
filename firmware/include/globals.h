/**********************************
 * header - global variables & defines
 *
 * Author: F00K42
 * Last change: 2026/02/14
 ***********************************/

#ifndef _GLOBALS_H_
#define _GLOBALS_H_

#include <stdio.h>
#include <stdint.h>
#include "pico/stdlib.h"


/////
#define LCD_LINE_COUNT  (LCD_ROWS)
#define LCD_LINE_SIZE   (LCD_COLS)

// Spur auf dem der Lesekopf beim Start/Reset stehen soll
// Track 17 --> Directory (Tracks 0..41 !)
#define DIRECTORY_TRACK (17)
#define INIT_TRACK      DIRECTORY_TRACK
#define MENU_DATA_TRACK (16)
#define SELECTOR_TRACK  (18)

#define PRGFILE_TRACK   (16)  // we have Tracks 0..16 to store a PRG file into
#define SCRATCH_TRACK   (18)  // we use Tracks 18..41 for preparing the PRG data

#define NUM_TRACKS_MAX  (42)
#define NUM_TRACKS_STD  (35)
#define NUM_SPEEDZONES  (4)

#define GCR_SYNCMARK    (0xFF)
// *** definition of GCR-structure
// see: https://ist.uwaterloo.ca/~schepers/formats/G64.TXT

#define G64_VERSION     (0x00)
#define G64_TRACKCOUNT  NUM_TRACKS_MAX
#define G64_TRACKSIZE   (7928)  // = 0x1ef8

extern const uint16_t d64_track_offset[NUM_TRACKS_MAX];
extern const uint8_t d64_sector_count[NUM_SPEEDZONES];
// zone (3 - density) a track is played at: from the G64 speed table or a
// density rule. The D64 layout (sectors per track, gaps, interleave) always
// follows the track number: default_d64_track_zone.
extern uint8_t d64_track_zone[NUM_TRACKS_MAX];
extern const uint8_t default_d64_track_zone[NUM_TRACKS_MAX];

#define D64_SECTOR_SIZE (256)
extern uint8_t d64_sector_puffer[21*D64_SECTOR_SIZE+5];

extern const uint8_t g64_head[12];
extern uint32_t g64_jumptable[2*G64_TRACKCOUNT];
extern uint32_t g64_speedtable[2*G64_TRACKCOUNT];

#define G64_HEADERSIZE (sizeof(g64_head)+sizeof(g64_jumptable)+sizeof(g64_speedtable)) // should be 684 bytes

extern const uint8_t g64_speedzones[NUM_SPEEDZONES];
extern const uint16_t g64_nominal_tracksize[NUM_SPEEDZONES];

extern uint16_t g64_tracklen[G64_TRACKCOUNT];

// enough space to store 42 tracks, each 7928 bytes byte length
extern uint8_t g64_tracks[G64_TRACKCOUNT][G64_TRACKSIZE];

// half-tracks (n.5) of a G64 image are stored in a pool with their real
// length - only the ones present in the image (copy protections)
#define G64_HALFTRACK_POOL_SIZE (80*1024)
extern uint8_t  g64_halftrack_pool[G64_HALFTRACK_POOL_SIZE];
extern uint32_t g64_halftrack_pool_used;
extern uint8_t* g64_halftrack[G64_TRACKCOUNT];      // data of track n+1.5 (index n), NULL if none
extern uint16_t g64_halftracklen[G64_TRACKCOUNT];
void clear_halftracks(void);

// head position (0 = track 1, 1 = track 1.5, ...) -> key of the stored track
// the head reads: the half-track if it is stored, else the track below
static inline uint8_t g64_track_key(const uint8_t half_track)
{
    return ((half_track & 1) && (NULL != g64_halftrack[half_track >> 1])) ? half_track : (uint8_t) (half_track & 0xFE);
}

// raw data and length of a stored track (key from g64_track_key)
static inline uint8_t* g64_track_data(const uint8_t key, uint16_t* len)
{
    if (key & 1)
    {
        *len = g64_halftracklen[key >> 1];
        return g64_halftrack[key >> 1];
    }
    *len = g64_tracklen[key >> 1];
    return g64_tracks[key >> 1];
}

extern uint8_t id1;    // id1 and id2 identify a floppy-disk
extern uint8_t id2;    // - these need to change to .. signal a disk-change or after a format...

enum {UNDEF_IMAGE, G64_IMAGE, D64_IMAGE, PRG_IMAGE, TXT_IMAGE, SELECTOR_IMAGE};

extern const int64_t bytetimer_values[NUM_SPEEDZONES];

extern const uint8_t d64_sector_gap[NUM_SPEEDZONES];

#define HEADER_GAP_BYTES (9)

// Zeit die nach der letzten Stepperaktivität vergehen muss, um einen neuen Track von SD Karte zu laden
// (1541 Original Rom schaltet STP1 alle 15ms)
// Default 15
#define STEPPER_DELAY_TIME (5)

extern bool block_data_changes;

extern const char* version_str;
#endif
