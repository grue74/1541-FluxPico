/**********************************
 * header - bit-level resolution of raw GCR tracks (G64)
 *
 * The 1541 read electronics (UE7/UF4 counters, UD2 shift register,
 * UE3 bit counter) turn the flux bit-stream of a track into SYNC and
 * BYTE-READY events. On a G64 the bytes of the stream are NOT
 * necessarily aligned to the bytes the drive sees: after every SYNC
 * the bit counter restarts, so protected disks (and also some normal
 * ones) have syncs ending at arbitrary bit positions.
 *
 * gcr_resolve() simulates the read circuit for one revolution
 * (model taken from VICE src/drive/rotation.c, rotation_1541_gcr(),
 *  1541 circuitry simulation by Istvan Fabian and Benjamin 'BeRo' Rosseaux,
 *  VICE is GPL-2.0-or-later)
 * and stores only the *framing* of the track: a list of segments,
 * each consisting of a SYNC phase followed by bytes every 8 bitcells.
 * The byte values themselves are fetched from the raw track at
 * playback time (gcr_fetch_byte), so writes to the raw track become
 * visible immediately and the memory footprint stays small.
 *
 * This file has no Pico dependencies - it is also compiled by the
 * host test in firmware/tests.
 *
 * Last change: 2026/11/12
 ***********************************/

#ifndef _GCR_RESOLVE_H_
#define _GCR_RESOLVE_H_

#include <stdint.h>
#include <stdbool.h>

// maximum number of syncs per track that are tracked (each sync needs
// at least 11 bitcells; normal tracks have 2 syncs per sector -> ~42)
#define GCR_RES_MAX_SEGMENTS    (2048)

// marker for "sync never ends" (track consisting of 1-bits only)
#define GCR_RES_NO_OFFSET       (0xFFFFu)

// one revolution of a disk at 300rpm in microseconds
#define GCR_REVOLUTION_US       (200000u)

typedef struct {
    uint16_t sync_on;   // offset of the bitcell completing 10 one-bits -> SYNC asserted
    uint16_t sync_off;  // offset of first bitcell ending the sync (= 1st bit of next byte)
} gcr_segment_t;

typedef struct {
    uint32_t nbits;     // number of bitcells of the track (= 8 * tracklength)
    uint32_t origin;    // raw bit position of offset 0 (= sync_on of segment 0)
    uint16_t nseg;      // number of segments; 0 = track without any sync
    bool     overflow;  // more syncs on the track than GCR_RES_MAX_SEGMENTS
    uint8_t  track;     // half-track key this framing belongs to (set by caller)
    uint32_t image_gen; // image generation this framing belongs to (set by caller)
    uint32_t req_gen;   // resolve request this framing satisfies (set by caller)
    gcr_segment_t seg[GCR_RES_MAX_SEGMENTS];
} gcr_resolved_t;

// event types produced while the disk spins
enum {
    GCR_EV_SYNC_ON  = 0,    // SYNC line goes active (low)
    GCR_EV_SYNC_OFF = 1,    // SYNC line goes inactive
    GCR_EV_BYTE     = 2     // byte complete -> BYTE READY, data = 8 bits ending at offset
};

// playback cursor - points to the next event
typedef struct {
    uint32_t off;       // offset (relative to origin) of the event
    uint32_t rev;       // revolution counter (incremented when wrapping)
    uint16_t seg;       // segment the event belongs to
    uint8_t  type;      // GCR_EV_*
} gcr_cursor_t;

// resolve a raw track (len_bytes bytes, MSB first) into its framing.
// returns the number of segments found.
uint16_t gcr_resolve(const uint8_t* raw, uint32_t len_bytes, gcr_resolved_t* out);

// position the cursor at the first event with offset > off (0 <= off < nbits).
// returns true when the SYNC line is active at offset off.
bool gcr_cursor_seek(const gcr_resolved_t* res, gcr_cursor_t* cur, uint32_t off);

// advance the cursor to the next event
void gcr_cursor_next(const gcr_resolved_t* res, gcr_cursor_t* cur);

// byte boundary for writing: the first offset >= off where the bit counter
// of the read framing completes a byte (inside a sync the counter is held
// at 0, so it is off+8). The result can be >= nbits (next revolution).
uint32_t gcr_write_boundary(const gcr_resolved_t* res, uint32_t off);

// convert an offset into a raw bit position of the track
static inline uint32_t gcr_raw_pos(const gcr_resolved_t* res, uint32_t off)
{
    uint32_t q = res->origin + off;
    if (q >= res->nbits) { q -= res->nbits; }
    return q;
}

// fetch the byte the 1541 shift register holds when bit q (raw position)
// has been shifted in. Applies the UF4 behaviour for runs of 0-bits:
// the 4th zero in a row reads as 1, beyond that random flux reversals
// (weak bits) with the probabilities of the read circuit at that density
// (0..3, the density the track is played at). rng is a xorshift32 state.
uint8_t gcr_fetch_byte(const uint8_t* raw, uint32_t nbits, uint32_t q, uint32_t* rng, uint8_t density);

// write a byte into the raw track; its last (8th) bit goes to raw position q
void gcr_write_byte(uint8_t* raw, uint32_t nbits, uint32_t q, uint8_t value);


// ---------------------------------------------------------------
// reading a track at another density than it was recorded with
//
// A protection loader can select a density (VIA2 PB5/PB6) that does not
// match the rate a track was written at - RapidLok reads its key track at
// density 3. The 1541 decoder then clocks the flux at its own rate (more or
// fewer bits per revolution, phantom/random bits in long gaps).
// gcr_resample() runs the read circuit model (VICE rotation_1541_gcr(), see
// above) over one revolution of a recorded track at the given density and
// returns the bit stream the drive's shift register sees. Played back at
// its own length (200ms per revolution) this gives the same bytes.

// out: up to out_max bytes. density 0..3 (3 = fastest clock, tracks 1-17).
// returns the number of bytes written; *rot_bits is the output bit that
// belongs to input bit 0 (to keep the angle: see gcr_rotate_copy)
uint32_t gcr_resample(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t out_max, uint8_t density, uint32_t* rot_bits);

// the same with another seed for the random flux reversals in the gaps
// (gcr_resample uses GCR_RESAMPLE_SEED; the host tests try many seeds)
#define GCR_RESAMPLE_SEED       (0x1541D3A5u)
uint32_t gcr_resample_seeded(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t out_max, uint8_t density, uint32_t seed, uint32_t* rot_bits);

// dst bit i = src bit (i + rot_bits) of a track of nbytes bytes
void gcr_rotate_copy(uint8_t* dst, const uint8_t* src, uint32_t nbytes, uint32_t rot_bits);

#endif
