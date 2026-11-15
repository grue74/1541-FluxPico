/**********************************
 * host test for the disk emulation in drive.c
 *
 * drive.c is compiled against the stub SDK headers in tests/stubs with a
 * fake timer and recorded GPIOs. The alarm callback is driven by jumping
 * the fake time to each requested target; service_track_resolver() is
 * called every RESOLVE_LATENCY_US (like the main loop on core0).
 *
 * build (MSVC, from firmware/tests):
 *   cl /O2 /W3 /std:c11 /D_CRT_SECURE_NO_WARNINGS /Istubs /I..\include
 *      drive_harness.c ..\src\drive.c ..\src\globals.c ..\src\gcr_resolve.c ..\src\rw_routines.c ..\src\gcr.c
 *
 * scenarios:
 *  1. playback of DOS formatted tracks (all zones): all sectors decode,
 *     byte intervals match the track density
 *  2. DOS write of a sector into the directory track (like the selector
 *     does), then convert_gcr2d64track() and playback must return it
 *  3. switch to write mode inside a sync and on an all-$FF track:
 *     BYTE READY must start within one byte time
 *  4. stepping keeps the angular position; a track without data idles
 ***********************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "drive.h"
#include "gcr.h"
#include "gcr_resolve.h"
#include "rw_routines.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; printf("  FAIL: " __VA_ARGS__); printf("\n"); } else { printf("  ok: " __VA_ARGS__); printf("\n"); } } while (0)

/////////////////////////////////////////////////////////////////////
// globals normally defined in main.c and other modules

volatile uint16_t akt_track_pos = 0;
uint8_t selected_track;
volatile uint8_t akt_half_track;
uint8_t old_half_track;
uint8_t num_max_tracks;
bool is_image_mount = false;
bool floppy_wp = true;
uint8_t stepper_signal_puffer[256];
volatile uint8_t stepper_signal_r_pos = 0;
volatile uint8_t stepper_signal_w_pos = 0;
alarm_id_t stepper_alarm = 0;
volatile bool track_is_written = false;
volatile bool send_byte_ready = true;
volatile bool beeper_enabled = true;
uint8_t current_stepper_phase(void) { return (uint8_t) ((selected_track + 2) & 3); }
volatile uint8_t  track_write_nr = 0;
volatile uint16_t track_write_pos = 0;
uint8_t akt_image_type = UNDEF_IMAGE;
FATFS fs;
FIL   fd;
MENU_STRUCT image_menu;

static void d_void(void) {}
static void d_cursor(const uint8_t c, const uint8_t r) { (void) c; (void) r; }
static void d_str(const char* s) { (void) s; }
void (*display_clear)(void) = d_void;
void (*display_home)(void) = d_void;
void (*display_setcursor)(const uint8_t column, const uint8_t row) = d_cursor;
void (*display_string)(const char* char_array) = d_str;

void close_disk_image(FIL* f) { (void) f; }
void menu_set_entry_var1(MENU_STRUCT* m, const uint8_t id, const uint8_t v) { (void) m; (void) id; (void) v; }
char* dez2out(int32_t v, uint8_t n, char* s) { (void) v; (void) n; s[0] = 0; return s; }
sd_card_t* sd_get_by_num(int n) { (void) n; return NULL; }

// menu_image.c functions used by rw_routines.c (PRG loading, not tested here)
void generate_empty_image(const uint8_t a, const uint8_t b, const uint8_t c) { (void) a; (void) b; (void) c; }
void generate_bam(const char* const n, const uint8_t* const i) { (void) n; (void) i; }
void generate_directory_entry(const uint8_t* f, const uint8_t t, const uint8_t dt, const uint8_t ds, const uint16_t s) { (void) f; (void) t; (void) dt; (void) ds; (void) s; }

// FatFS on top of host files (FIL.host_file is a FILE*)
FRESULT f_read(FIL* f, void* b, UINT n, UINT* r) { if (!f->host_file) { *r = 0; return 1; } *r = (UINT) fread(b, 1, n, (FILE*) f->host_file); return FR_OK; }
FRESULT f_write(FIL* f, const void* b, UINT n, UINT* w) { if (!f->host_file) { *w = 0; return 1; } *w = (UINT) fwrite(b, 1, n, (FILE*) f->host_file); return FR_OK; }
FRESULT f_open(FIL* f, const char* name, int mode) { (void) mode; memset(f, 0, sizeof(*f)); f->host_file = fopen(name, "rb"); return f->host_file ? FR_OK : 4; }
FRESULT f_close(FIL* f) { if (f->host_file) { fclose((FILE*) f->host_file); f->host_file = NULL; } return FR_OK; }
FRESULT f_lseek(FIL* f, uint32_t o) { if (!f->host_file) { return 1; } return fseek((FILE*) f->host_file, (long) o, SEEK_SET) ? 1 : FR_OK; }

/////////////////////////////////////////////////////////////////////
// fake hardware

static uint64_t fake_now = 1000000;
static bool in_oe = true;       // true = read mode
static bool in_soe = true;
static bool in_motor = true;
static uint8_t pa_in = 0;
static uint8_t pa_out = 0;
static bool brdy_out = false;
static bool sync_level = true;

typedef struct { uint64_t t; char kind; uint8_t data; } logev_t;    // 'B' byte ready, 'S' sync low, 's' sync high
#define MAXLOG (400000)
static logev_t logbuf[MAXLOG];
static int nlog = 0;

static void (*on_byte_ready)(uint64_t t, uint8_t pa_value) = NULL;
static void (*on_sync)(uint64_t t, bool low) = NULL;

static void log_ev(char k, uint8_t d) { if (nlog < MAXLOG) { logbuf[nlog].t = fake_now; logbuf[nlog].kind = k; logbuf[nlog].data = d; ++nlog; } }

void gpio_init(uint p) { (void) p; }
void gpio_set_pulls(uint p, bool u, bool d) { (void) p; (void) u; (void) d; }
void gpio_init_mask(uint32_t m) { (void) m; }
void gpio_set_dir_in_masked(uint32_t m) { (void) m; }
void gpio_set_dir_out_masked(uint32_t m) { (void) m; }
void gpio_set_drive_strength(uint p, int s) { (void) p; (void) s; }
void gpio_set_irq_enabled(uint p, uint32_t e, bool en) { (void) p; (void) e; (void) en; }
void gpio_set_dir(uint p, bool out) { if (GPIO_BRDY == p) { brdy_out = out; } }
void gpio_put(uint p, bool v)
{
    if ((GPIO_BRDY == p) && brdy_out && !v)
    {
        log_ev(in_oe ? 'B' : 'W', in_oe ? pa_out : pa_in);
        if (on_byte_ready) { on_byte_ready(fake_now, in_oe ? pa_out : pa_in); }
    }
    if ((GPIO_SYNC == p) && (v != sync_level))
    {
        sync_level = v;
        log_ev(v ? 's' : 'S', 0);
        if (on_sync) { on_sync(fake_now, !v); }
    }
}
bool gpio_get(uint p)
{
    if (GPIO_SOE == p) { return in_soe; }
    if (GPIO_OE  == p) { return in_oe; }
    if (GPIO_MTR == p) { return in_motor; }
    return false;
}
void gpio_put_masked(uint32_t m, uint32_t v) { (void) m; pa_out = (uint8_t) (v >> GPIO_PAPORT); }
uint32_t gpio_get_all(void) { return ((uint32_t) pa_in) << GPIO_PAPORT; }

uint64_t time_us_64(void) { return fake_now; }
static bool busy_advances = false;      // busy waits take time (like on the Pico)
void busy_wait_us(uint64_t us) { if (busy_advances) { fake_now += us; } }
void sleep_ms(uint32_t ms) { (void) ms; }
absolute_time_t from_us_since_boot(uint64_t us) { return us; }

static alarm_callback_t alarm_cb = NULL;
static uint64_t alarm_t = 0;
static bool alarm_on = false;

alarm_id_t add_alarm_at(absolute_time_t t, alarm_callback_t cb, void* ud, bool past) { (void) ud; (void) past; alarm_cb = cb; alarm_t = t; alarm_on = true; return 1; }
alarm_id_t add_alarm_in_ms(uint32_t ms, alarm_callback_t cb, void* ud, bool past) { (void) ms; (void) cb; (void) ud; (void) past; return 2; }
bool cancel_alarm(alarm_id_t id) { if (1 == id) { alarm_on = false; } return true; }

/////////////////////////////////////////////////////////////////////

static uint64_t resolve_latency_us = 3000;     // main loop resolve delay
static uint64_t next_resolve = 0;

// advance fake time to t_end, firing the disk alarm and the "main loop"
static void run_until(uint64_t t_end)
{
    while (alarm_on && (alarm_t <= t_end))
    {
        if (alarm_t >= next_resolve)
        {
            fake_now = next_resolve;
            service_track_resolver();
            next_resolve += resolve_latency_us;
        }
        if (alarm_t < fake_now) { printf("  time went backwards\n"); ++failures; }
        fake_now = alarm_t;
        int64_t d = alarm_cb(1, NULL);
        if (d < 0) { alarm_t += (uint64_t) (-d); } else if (d > 0) { alarm_t = fake_now + d; } else { alarm_on = false; }
    }
    fake_now = t_end;
}

/////////////////////////////////////////////////////////////////////
// disk content

static uint8_t sector_byte(int track, int sector, int i) { return (uint8_t) (track * 31 + sector * 7 + i); }

static void build_track(int track_nr)    // track_nr 0-based
{
    const int nsec = d64_sector_count[d64_track_zone[track_nr]];
    for (int s = 0; s < nsec; ++s)
    {
        for (int i = 0; i < 256; ++i) { d64_sector_puffer[s * 256 + 1 + i] = sector_byte(track_nr, s, i); }
    }
    convert_d64track2gcr((uint8_t) track_nr, 'A', 'B');
}

static uint8_t stepper_phase = 0;

// move the head by one half-track like the 1541 stepper does
static void step_half(int dir)
{
    // like the 1541: the phase that belongs to the current head position +-1
    stepper_phase = (uint8_t) ((selected_track + 2 + (dir > 0 ? 1 : 3)) & 3);
    stepper_signal_puffer[stepper_signal_w_pos] = stepper_phase;
    ++stepper_signal_w_pos;
    check_stepper_signals();
}

// feed one stepper phase as the drive outputs it
static void put_phase(uint8_t phase)
{
    stepper_signal_puffer[stepper_signal_w_pos] = (uint8_t) (phase & 3);
    ++stepper_signal_w_pos;
    check_stepper_signals();
}

// scenario: DOS bump (90 half-steps out from track 18 against the end stop),
// then back to track 18. A wrong start position (half a track off) heals.
static void scenario_bump(void)
{
    printf("8. stepper: bump and absolute phase\n");
    selected_track = 34; akt_half_track = 34;
    uint8_t ph = 0;                                 // phase of track 18
    for (int k = 0; k < 90; ++k) { ph = (uint8_t) ((ph + 3) & 3); put_phase(ph); }
    CHECK(0 == selected_track, "bump ends on track 1 (position %d)", selected_track);
    for (int k = 0; k < 34; ++k) { ph = (uint8_t) ((ph + 1) & 3); put_phase(ph); }
    CHECK(34 == selected_track, "34 half-steps in: track 18.0 (position %d)", selected_track);
    // start half a track off (old firmware could end up like this): the next
    // phases still move the head, and a bump puts it back in line
    selected_track = 35; akt_half_track = 35;
    ph = 0;
    for (int k = 0; k < 90; ++k) { ph = (uint8_t) ((ph + 3) & 3); put_phase(ph); }
    for (int k = 0; k < 34; ++k) { ph = (uint8_t) ((ph + 1) & 3); put_phase(ph); }
    CHECK(34 == selected_track, "after a bump the head is in line again (position %d)", selected_track);
}

// step the head to half-track position half
static void head_to(uint8_t half)
{
    while (selected_track < half) { step_half(+1); }
    while (selected_track > half) { step_half(-1); }
}

static void reset_stepper(void)
{
    stepper_signal_r_pos = stepper_signal_w_pos = 0;
    stepper_phase = 0;
    stepper_signal_puffer[255] = 0;
}

static void mount_d64_like(void)
{
    reset_stepper();
    memcpy(d64_track_zone, default_d64_track_zone, sizeof(default_d64_track_zone));
    memset(g64_tracklen, 0, sizeof(g64_tracklen));
    for (int t = 0; t < 35; ++t) { build_track(t); }
    is_image_mount = true;
    in_motor = true; in_oe = true; in_soe = true;
    selected_track = DIRECTORY_TRACK << 1;
    akt_half_track = selected_track;
    start_bytetimer(akt_half_track);
    next_resolve = fake_now;
}

/////////////////////////////////////////////////////////////////////
// decode sectors from the log, starting at log index from

typedef struct { int header_ok; int track; int sector; int data_ok; int data_sector_guess; uint8_t data[256]; } sec_t;

static int gcr_decode_block(const uint8_t* g, int n5, uint8_t* out)
{
    for (int i = 0; i < n5; ++i) { ConvertFromGCR(&g[i * 5], &out[i * 4]); }
    return 1;
}

// returns number of good data blocks; found[] marks sectors (via preceding header)
static int decode_log(int from, int to, int track1, uint32_t* found_mask, uint8_t sector_data[21][256])
{
    int good = 0, last_sector = -1;
    *found_mask = 0;
    for (int i = from; i < to; ++i)
    {
        if (logbuf[i].kind != 's') { continue; }
        uint8_t g[400]; int n = 0;
        for (int j = i + 1; j < to && logbuf[j].kind == 'B' && n < 400; ++j) { g[n++] = logbuf[j].data; }
        uint8_t d[320];
        if (n >= 10)
        {
            gcr_decode_block(g, 2, d);
            if (d[0] == 0x08 && d[3] == track1 && d[1] == (d[2] ^ d[3] ^ d[4] ^ d[5])) { last_sector = d[2]; continue; }
        }
        if (n >= 325 && last_sector >= 0)
        {
            gcr_decode_block(g, 65, d);
            uint8_t sum = 0;
            for (int b = 1; b <= 256; ++b) { sum ^= d[b]; }
            if (d[0] == 0x07 && sum == d[257])
            {
                *found_mask |= 1u << last_sector;
                if (sector_data) { memcpy(sector_data[last_sector], &d[1], 256); }
                ++good;
            }
            last_sector = -1;
        }
    }
    return good;
}

/////////////////////////////////////////////////////////////////////

static void scenario_playback(void)
{
    printf("1. playback of DOS formatted tracks\n");
    static const int tracks[4] = { 1, 18, 25, 35 };
    mount_d64_like();
    for (int z = 0; z < 4; ++z)
    {
        const int t = tracks[z] - 1;
        head_to((uint8_t) (t << 1));
        run_until(fake_now + 20000);        // settle + resolve
        int from = nlog;
        run_until(fake_now + 2 * GCR_REVOLUTION_US);
        uint32_t mask;
        int nsec = d64_sector_count[d64_track_zone[t]];
        decode_log(from, nlog, t + 1, &mask, NULL);
        CHECK(mask == (1u << nsec) - 1, "track %d: all %d sectors decode (mask %06X)", t + 1, nsec, mask);
        CHECK(disk_header_sector < nsec && (fake_now / 1000 - disk_header_ms) < 20,
              "track %d: display sees sector headers (last %u, %u ms ago)", t + 1, disk_header_sector, (unsigned) (fake_now / 1000 - disk_header_ms));

        // byte intervals inside data blocks
        double sum = 0; int cnt = 0; double mn = 1e9, mx = 0;
        for (int i = from + 1; i < nlog; ++i)
        {
            if (logbuf[i].kind == 'B' && logbuf[i - 1].kind == 'B')
            {
                double d = (double) (logbuf[i].t - logbuf[i - 1].t);
                sum += d; ++cnt; if (d < mn) mn = d; if (d > mx) mx = d;
            }
        }
        double expect = 8.0 * GCR_REVOLUTION_US / (g64_tracklen[t] * 8.0);
        CHECK(cnt > 0 && sum / cnt > expect - 0.05 && sum / cnt < expect + 0.05 && mn >= expect - 1.01 && mx <= expect + 1.01,
              "track %d: byte interval avg %.3fus (min %.0f max %.0f), expected %.3fus", t + 1, sum / cnt, mn, mx, expect);
    }
}

/////////////////////////////////////////////////////////////////////
// scenario 2: DOS-like write of sector 2 of the directory track

static enum { DOS_IDLE, DOS_HEADER, DOS_GAP, DOS_WRITE, DOS_DONE } dos_state;
static int dos_count;
static uint8_t dos_hdr[10];
static uint8_t dos_out[5 + 325];
static int dos_target_sector;
static int dos_next_target = -1;    // sector to write right after this one (-1: none)
static uint8_t dos_out_next[5 + 325];

static void dos_sync(uint64_t t, bool low)
{
    (void) t;
    if (!low && DOS_IDLE == dos_state) { dos_state = DOS_HEADER; dos_count = 0; }
}

static void dos_br(uint64_t t, uint8_t v)
{
    (void) t;
    switch (dos_state)
    {
        case DOS_HEADER:
            dos_hdr[dos_count++] = v;
            if (10 == dos_count)
            {
                uint8_t d[8];
                ConvertFromGCR(dos_hdr, d);
                if (d[0] == 0x08 && d[2] == dos_target_sector && d[3] == DIRECTORY_TRACK + 1) { dos_state = DOS_GAP; dos_count = 0; }
                else { dos_state = DOS_IDLE; }
            }
            break;
        case DOS_GAP:
            if (++dos_count == 9)
            {
                // switch to write mode, first byte on PA
                in_oe = false;
                dos_count = 0;
                pa_in = dos_out[0];
                dos_state = DOS_WRITE;
            }
            break;
        case DOS_WRITE:
            // byte dos_count has been taken, provide the next one
            if (++dos_count < (int) sizeof(dos_out)) { pa_in = dos_out[dos_count]; }
            else
            {
                in_oe = true;
                if (dos_next_target >= 0)
                {
                    // continue with the next sector right away
                    dos_target_sector = dos_next_target;
                    dos_next_target = -1;
                    memcpy(dos_out, dos_out_next, sizeof(dos_out));
                    dos_state = DOS_IDLE;
                } else {
                    dos_state = DOS_DONE;
                }
            }
            break;
        default:
            break;
    }
}

static void scenario_write(void)
{
    printf("2. DOS write into the directory track, read back\n");
    mount_d64_like();
    run_until(fake_now + 20000);

    // data block: 5x sync, GCR(07 + 256 data + checksum + 00 00)
    uint8_t blk[260];
    blk[0] = 0x07;
    uint8_t sum = 0;
    for (int i = 0; i < 256; ++i) { blk[1 + i] = (uint8_t) (0xA5 ^ i); sum ^= blk[1 + i]; }
    blk[257] = sum; blk[258] = 0; blk[259] = 0;
    memset(dos_out, 0xFF, 5);
    for (int i = 0; i < 65; ++i) { ConvertToGCR(&blk[i * 4], &dos_out[5 + i * 5]); }

    dos_target_sector = 2;
    dos_state = DOS_IDLE;
    on_sync = dos_sync; on_byte_ready = dos_br;
    track_is_written = false;
    run_until(fake_now + 2 * GCR_REVOLUTION_US);
    on_sync = NULL; on_byte_ready = NULL;
    CHECK(DOS_DONE == dos_state, "write sequence completed (state %d)", (int) dos_state);
    CHECK(track_is_written && track_write_nr == DIRECTORY_TRACK, "track_is_written set for track %d", track_write_nr + 1);
    CHECK(disk_write_sector == 2, "display reports sector %u as written", disk_write_sector);

    // what files.c does for the selector
    run_until(fake_now + 10000);
    convert_gcr2d64track(DIRECTORY_TRACK);
    CHECK(0 == memcmp(&d64_sector_puffer[1 + 2 * D64_SECTOR_SIZE], &blk[1], 256), "convert_gcr2d64track returns the written sector");

    // and the drive reads it back (DOS verify)
    int from = nlog;
    run_until(fake_now + 2 * GCR_REVOLUTION_US);
    static uint8_t secs[21][256];
    uint32_t mask;
    decode_log(from, nlog, DIRECTORY_TRACK + 1, &mask, secs);
    CHECK(mask == (1u << 19) - 1, "all 19 sectors still decode after the write (mask %06X)", mask);
    CHECK(0 == memcmp(secs[2], &blk[1], 256), "sector 2 reads back the written data");
    int same = 1;
    for (int k = 0; k < 256; ++k) { same &= (secs[3][k] == sector_byte(DIRECTORY_TRACK, 3, k)); }
    CHECK(same, "sector 3 untouched");
}

/////////////////////////////////////////////////////////////////////
// scenario 3: write mode switched on inside a sync

static uint64_t sw_time;
static int sw_armed;
static void sw_sync(uint64_t t, bool low)
{
    if (low && sw_armed) { sw_armed = 0; in_oe = false; pa_in = 0x55; sw_time = t; }
}

static void scenario_switch_in_sync(void)
{
    printf("3. write mode switched on inside a sync\n");
    int from, i;

    // all-$FF track: sync everywhere, no event for a whole revolution
    mount_d64_like();
    memset(g64_tracks[DIRECTORY_TRACK], 0xFF, G64_TRACKSIZE);
    g64_tracklen[DIRECTORY_TRACK] = 7142;
    start_bytetimer(akt_half_track);
    run_until(fake_now + 20000);
    from = nlog;
    sw_time = fake_now + 12345;
    run_until(sw_time);
    in_oe = false; pa_in = 0xFF;
    run_until(fake_now + 1000);
    i = from;
    while (i < nlog && logbuf[i].kind != 'W') { ++i; }
    CHECK(i < nlog && (logbuf[i].t - sw_time) <= 40, "all-$FF track: first BYTE READY %llu us after OE low",
          (unsigned long long) (i < nlog ? logbuf[i].t - sw_time : 999999));
    in_oe = true;

    // inside a sync of a normal track
    mount_d64_like();
    run_until(fake_now + 20000);
    sw_armed = 1; on_sync = sw_sync;
    from = nlog;
    run_until(fake_now + 30000);
    on_sync = NULL;
    i = from;
    while (i < nlog && !(logbuf[i].t >= sw_time && logbuf[i].kind == 'W')) { ++i; }
    CHECK(i < nlog && (logbuf[i].t - sw_time) <= 40, "normal track, OE low at sync start: first BYTE READY %llu us later",
          (unsigned long long) (i < nlog ? logbuf[i].t - sw_time : 999999));
    // written 0x55 bytes from there on until OE goes high again
    in_oe = true;
    run_until(fake_now + 10000);
}

/////////////////////////////////////////////////////////////////////
// scenario 4: stepping keeps the angle; empty track idles

static void scenario_step(void)
{
    printf("4. stepping\n");
    mount_d64_like();
    run_until(fake_now + 20000);

    // step from track 18 to 19 at an odd time
    run_until(fake_now + 77777);
    head_to((DIRECTORY_TRACK + 1) << 1);
    int from = nlog;
    run_until(fake_now + 20000);
    // first byte after arrival: compare with the raw track at that angle
    int i = from;
    while (i < nlog && logbuf[i].kind != 'B') { ++i; }
    int ok = 0;
    if (i < nlog)
    {
        const int t = DIRECTORY_TRACK + 1;
        const uint32_t nbits = g64_tracklen[t] * 8;
        const uint64_t ang = logbuf[i].t % GCR_REVOLUTION_US;
        const uint32_t q = (uint32_t) (ang * nbits / GCR_REVOLUTION_US);
        // byte ends at raw bit position ~q (byte aligned on these tracks)
        for (int d = -2; d <= 2 && !ok; ++d)
        {
            uint32_t qq = (q + nbits + d) % nbits;
            if ((qq & 7) == 7 && g64_tracks[t][qq >> 3] == logbuf[i].data) { ok = 1; }
        }
        CHECK(ok, "first byte after step (t=%llu, data %02X) matches track 19 at angle %.4f",
              (unsigned long long) logbuf[i].t, logbuf[i].data, (double) ang / GCR_REVOLUTION_US);
    } else {
        CHECK(0, "no byte after step");
    }

    // a track without data (tracklen 0)
    g64_tracklen[39] = 0;
    head_to(39 << 1);
    from = nlog;
    run_until(fake_now + 50000);
    int brs = 0;
    for (i = from; i < nlog; ++i) { brs += (logbuf[i].kind == 'B'); }
    CHECK(brs == 0, "empty track: no byte ready, no crash (%d)", brs);
}

/////////////////////////////////////////////////////////////////////
// scenario 5: two sectors written back to back (next header ~0.6ms after
// the first write) while the framing is still being resolved

static void make_block(uint8_t seed, uint8_t* blk, uint8_t* out)
{
    blk[0] = 0x07;
    uint8_t sum = 0;
    for (int i = 0; i < 256; ++i) { blk[1 + i] = (uint8_t) (seed + i * 3); sum ^= blk[1 + i]; }
    blk[257] = sum; blk[258] = 0; blk[259] = 0;
    memset(out, 0xFF, 5);
    for (int i = 0; i < 65; ++i) { ConvertToGCR(&blk[i * 4], &out[5 + i * 5]); }
}

static void scenario_two_writes(void)
{
    printf("5. sectors 2 and 3 written back to back (resolve latency %llu us)\n", (unsigned long long) resolve_latency_us);
    mount_d64_like();
    run_until(fake_now + 20000);

    static uint8_t blk2[260], blk3[260];
    make_block(0x11, blk2, dos_out);
    make_block(0x77, blk3, dos_out_next);
    dos_target_sector = 2;
    dos_next_target = 3;
    dos_state = DOS_IDLE;
    on_sync = dos_sync; on_byte_ready = dos_br;
    int from = nlog;
    run_until(fake_now + 3 * GCR_REVOLUTION_US);
    on_sync = NULL; on_byte_ready = NULL;
    CHECK(DOS_DONE == dos_state, "both writes completed (state %d)", (int) dos_state);

    // byte ready never closer than a byte time (minus rounding)
    const double bt = 8.0 * GCR_REVOLUTION_US / (g64_tracklen[DIRECTORY_TRACK] * 8.0);
    uint64_t prev = 0, mind = 999999;
    int at = from;
    for (int i = from; i < nlog; ++i)
    {
        if (logbuf[i].kind != 'B' && logbuf[i].kind != 'W') { continue; }
        if (prev && (logbuf[i].t - prev) < mind) { mind = logbuf[i].t - prev; at = i; }
        prev = logbuf[i].t;
    }
    if ((double) mind < bt - 2.0)
    {
        for (int i = (at > from + 6) ? at - 6 : from; i < at + 4 && i < nlog; ++i)
        {
            printf("    %llu %c %02X\n", (unsigned long long) logbuf[i].t, logbuf[i].kind, logbuf[i].data);
        }
    }
    CHECK((double) mind >= bt - 2.0, "minimum BYTE READY distance %llu us (byte time %.2f us)", (unsigned long long) mind, bt);

    run_until(fake_now + 20000);
    convert_gcr2d64track(DIRECTORY_TRACK);
    CHECK(0 == memcmp(&d64_sector_puffer[1 + 2 * D64_SECTOR_SIZE], &blk2[1], 256), "sector 2 written");
    CHECK(0 == memcmp(&d64_sector_puffer[1 + 3 * D64_SECTOR_SIZE], &blk3[1], 256), "sector 3 written");

    from = nlog;
    run_until(fake_now + 2 * GCR_REVOLUTION_US);
    static uint8_t secs[21][256];
    uint32_t mask;
    decode_log(from, nlog, DIRECTORY_TRACK + 1, &mask, secs);
    CHECK(mask == (1u << 19) - 1 && 0 == memcmp(secs[2], &blk2[1], 256) && 0 == memcmp(secs[3], &blk3[1], 256),
          "read back: all sectors decode, 2 and 3 have the new data (mask %06X)", mask);
}

/////////////////////////////////////////////////////////////////////
// scenario 7: density rules (<image>.den) - RapidLok reads its key track
// 36 at density 3 and needs at least 256 bytes between the two syncs

static uint32_t bytes_after_sync(uint8_t track_nr)
{
    static gcr_resolved_t r;
    gcr_resolve(g64_tracks[track_nr], g64_tracklen[track_nr], &r);
    if ((1 != r.nseg) || (GCR_RES_NO_OFFSET == r.seg[0].sync_off)) { return 0; }
    return (r.nbits - r.seg[0].sync_off) / 8;
}

static int8_t load_g64_named(const char* path);

// longest run of data between two syncs (bytes) on a track; *seam_in: the
// raw bit 0 of the track (the seam of the conversion) lies inside that run
static uint32_t longest_run(const uint8_t* track, uint16_t len, int* seam_in)
{
    static gcr_resolved_t r;
    gcr_resolve(track, len, &r);
    *seam_in = 0;
    if ((0 == r.nseg) || r.overflow) { return 0; }
    const uint32_t seam = (r.nbits - r.origin) % r.nbits;     // offset of raw bit 0
    uint32_t best = 0;
    for (uint32_t i = 0; i < r.nseg; ++i)
    {
        if (GCR_RES_NO_OFFSET == r.seg[i].sync_off) { continue; }
        const uint32_t from = r.seg[i].sync_off;
        const uint32_t to   = (i + 1 < r.nseg) ? r.seg[i + 1].sync_on : r.nbits;   // seg 0 starts at offset 0
        const uint32_t run  = (to - from) / 8;
        if (run > best) { best = run; *seam_in = (seam >= from) && (seam < to); }
    }
    return best;
}

// the gaps without flux get their bits from random flux reversals: the
// conversion freezes one realisation (GCR_RESAMPLE_SEED). Try many seeds
// on the original key track to see the margin above 256 bytes.
static void density_seed_sweep(const uint8_t* orig, uint16_t orig_len)
{
    static uint8_t conv[G64_TRACKSIZE], rot[G64_TRACKSIZE];
    uint32_t lo = 0xFFFFFFFFu, hi = 0, sum = 0, below = 0, seam_hits = 0, def_run = 0;
    const int seeds = 200;
    for (int k = 0; k <= seeds; ++k)
    {
        // k == 0: the seed the firmware uses
        const uint32_t seed = (0 == k) ? GCR_RESAMPLE_SEED : (0x9E3779B9u * (uint32_t) k) ^ 0x2545F491u;
        uint32_t rb;
        const uint32_t n = gcr_resample_seeded(orig, orig_len, conv, G64_TRACKSIZE, 3, seed, &rb);
        gcr_rotate_copy(rot, conv, n, rb);
        int seam_in;
        const uint32_t run = longest_run(rot, (uint16_t) n, &seam_in);
        if (0 == k) { def_run = run; continue; }
        if (run < lo) { lo = run; }
        if (run > hi) { hi = run; }
        sum += run;
        below += (run < 256);
        seam_hits += seam_in;
    }
    printf("  seed sweep (%d seeds, density 3): key run %u..%u bytes, mean %.1f; firmware seed %u; below 256: %u; seam inside the run: %u\n",
           seeds, lo, hi, (double) sum / seeds, def_run, below, seam_hits);
    CHECK(0 == below, "every seed gives 256 or more bytes between the syncs (minimum %u)", lo);
    CHECK(def_run >= 256, "the firmware seed gives %u bytes", def_run);
}

static void scenario_density(const char* path)
{
    printf("7. density rules: %s\n", path);
    char den[1024];
    snprintf(den, sizeof(den), "%.*s.den", (int) strlen(path) - 4, path);
    remove(den);

    int8_t last = load_g64_named(path);
    CHECK(last >= 0, "image loads without rules");
    const uint16_t len_before = g64_tracklen[35];
    const uint32_t before = bytes_after_sync(35);
    printf("  without rules: track 36 %u bytes, %u bytes between the syncs\n", len_before, before);
    CHECK(before < 256, "without rules track 36 has fewer than 256 bytes between the syncs (%u)", before);
    {
        static uint8_t orig[G64_TRACKSIZE];
        memcpy(orig, g64_tracks[35], len_before);
        int seam_in;
        const uint32_t run = longest_run(orig, len_before, &seam_in);
        printf("  original track 36: longest run %u bytes, seam inside it: %s\n", run, seam_in ? "yes" : "no");
        density_seed_sweep(orig, len_before);
    }

    FILE* f = fopen(den, "w");
    fprintf(f, "# Law of the West (RapidLok 1)\n32-36 3\n");
    fclose(f);
    last = load_g64_named(path);
    remove(den);
    CHECK(last >= 0, "image loads with rules");
    const uint32_t after = bytes_after_sync(35);
    printf("  with rules:    track 36 %u bytes, %u bytes between the syncs, zone %u\n", g64_tracklen[35], after, d64_track_zone[35]);
    CHECK(after >= 256, "with rules track 36 has 256 or more bytes between the syncs (%u)", after);
    CHECK(0 == d64_track_zone[35] && 3 == g64_speedtable[35 * 2], "converted track is marked as density 3");
    CHECK(g64_tracklen[17] == 7146 || g64_tracklen[17] > 7000, "track 18 is not converted (%u bytes)", g64_tracklen[17]);

    // play the converted key track for some revolutions with busy waits that
    // take time: every revolution must deliver the same bytes between the
    // syncs (no drift, no resync in the long gap without flux)
    is_image_mount = true;
    in_motor = true; in_oe = true; in_soe = true;
    selected_track = 70; akt_half_track = 70;      // track 36
    busy_advances = true;
    start_bytetimer(70);
    next_resolve = fake_now;
    run_until(fake_now + 250000);
    const int from = nlog;
    run_until(fake_now + 1000000);
    busy_advances = false;
    int counts[8], nc = 0, bytes = -1;
    for (int i = from; i < nlog && nc < 8; ++i)
    {
        if ('S' == logbuf[i].kind) { if (bytes >= 0) { counts[nc++] = bytes; } bytes = 0; }
        else if (('B' == logbuf[i].kind) && (bytes >= 0)) { ++bytes; }
    }
    int same = (nc >= 3);
    for (int i = 1; i < nc; ++i) { same &= (counts[i] == counts[0]); }
    printf("  track 36 played: %d revolutions, bytes between the syncs:", nc);
    for (int i = 0; i < nc; ++i) { printf(" %d", counts[i]); }
    printf("\n");
    CHECK(same && counts[0] >= 256, "every revolution of track 36 has the same 256+ bytes between the syncs");

    // the D64 layout does not follow the converted zone
    CHECK(17 == d64_sector_count[default_d64_track_zone[31]] && 0 == d64_track_zone[31], "track 32 played at density 3, D64 layout keeps 17 sectors");

    // Save Image (G64) stores the converted tracks (speed 3). Loading that
    // file with the same .den must not convert them a second time.
    const uint16_t conv_len = g64_tracklen[35];
    const char* tmp = "drive_harness_den.g64";
    char tmp_den[64];
    snprintf(tmp_den, sizeof(tmp_den), "%.*s.den", (int) strlen(tmp) - 4, tmp);
    FILE* sf = fopen(tmp, "wb");
    FIL sfil; memset(&sfil, 0, sizeof(sfil)); sfil.host_file = sf;
    const int8_t w = write_disk(&sfil, G64_IMAGE, (uint8_t) (last + 1));
    fclose(sf);
    CHECK(w >= 0, "save converted image (result %d)", w);
    f = fopen(tmp_den, "w");
    fprintf(f, "32-36 3\n");
    fclose(f);
    last = load_g64_named(tmp);
    remove(tmp_den);
    remove(tmp);
    CHECK(last >= 0 && conv_len == g64_tracklen[35] && bytes_after_sync(35) == after,
          "saved image + .den loads without a second conversion (track 36 %u bytes, %u between the syncs)", g64_tracklen[35], bytes_after_sync(35));
}

/////////////////////////////////////////////////////////////////////
// scenario 6: a G64 with half-tracks, loaded by the firmware loader

// with the image name set, so read_disk finds "<name>.den" next to it
static int8_t load_g64_named(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) { return -100; }
    FIL fil; memset(&fil, 0, sizeof(fil)); fil.host_file = f;
    FILINFO fi; memset(&fi, 0, sizeof(fi));
    strncpy(fi.fname, path, sizeof(fi.fname) - 1);
    const int8_t last = read_disk(&fil, G64_IMAGE, fi);
    fclose(f);
    return last;
}

static int8_t load_g64(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) { return -100; }
    FIL fil; memset(&fil, 0, sizeof(fil)); fil.host_file = f;
    FILINFO fi; memset(&fi, 0, sizeof(fi));
    const int8_t last = read_disk(&fil, G64_IMAGE, fi);
    fclose(f);
    return last;
}


// bytes played between log index from and to must come from track key
static void check_played_track(int from, int to, uint8_t key, const char* what)
{
    uint16_t len;
    const uint8_t* raw = g64_track_data(key, &len);
    const uint32_t nbits = len * 8u;
    int match = 0, cmp = 0;
    for (int i = from; i < to; ++i)
    {
        if (logbuf[i].kind != 'B') { continue; }
        const uint32_t q = (uint32_t) ((logbuf[i].t % GCR_REVOLUTION_US) * nbits / GCR_REVOLUTION_US);
        int ok = 0, det = 0;
        for (int d = -1; d <= 1; ++d)
        {
            const uint32_t qq = (q + nbits + d) % nbits;
            uint32_t r1 = 1, r2 = 0x9E3779B9;
            const uint8_t b1 = gcr_fetch_byte(raw, nbits, qq, &r1, 3);
            const uint8_t b2 = gcr_fetch_byte(raw, nbits, qq, &r2, 3);
            if (b1 == b2)
            {
                det = 1;
                if (b1 == logbuf[i].data) { ok = 1; }
            }
        }
        if (det) { ++cmp; match += ok; }
    }
    CHECK(cmp > 100 && match * 100 >= cmp * 98, "%s: %d of %d bytes played match the track at their angle", what, match, cmp);
}

static void scenario_halftracks(const char* path)
{
    printf("6. half-tracks: %s\n", path);
    in_oe = true; in_soe = true; in_motor = true;
    const int8_t last = load_g64(path);
    CHECK(last >= 0, "read_disk loads the image (last track %d)", last + 1);
    if (last < 0) { return; }
    num_max_tracks = (uint8_t) (last + 1);

    int nhalf = 0;
    for (int i = 0; i < G64_TRACKCOUNT; ++i) { nhalf += (NULL != g64_halftrack[i]); }
    printf("  %d half-tracks loaded, pool %u of %u bytes\n", nhalf, (unsigned) g64_halftrack_pool_used, (unsigned) G64_HALFTRACK_POOL_SIZE);
    CHECK(nhalf > 0, "half-tracks present");

    // compare each loaded half-track with the file
    {
        FILE* f = fopen(path, "rb");
        static uint8_t img[1 << 20];
        const size_t sz = fread(img, 1, sizeof(img), f);
        fclose(f);
        int same = 1;
        for (int i = 0; i < G64_TRACKCOUNT; ++i)
        {
            uint32_t off; memcpy(&off, &img[12 + (i * 2 + 1) * 4], 4);
            if (!g64_halftrack[i]) { continue; }
            const uint32_t len = img[off] | (img[off + 1] << 8);
            same &= (off + 2 + len <= sz) && (len == g64_halftracklen[i]) && (0 == memcmp(g64_halftrack[i], &img[off + 2], len));
        }
        CHECK(same, "half-track data equals the file");
    }

    // head to track 1 (half-track position 0), then step through 1.0 - 1.5 - 2.0 - 2.5
    is_image_mount = true;
    reset_stepper();
    selected_track = 0;
    akt_half_track = 0;
    start_bytetimer(0);
    next_resolve = fake_now;
    run_until(fake_now + 20000);

    for (int pos = 0; pos < 4; ++pos)
    {
        if (pos > 0)
        {
            step_half(+1);
            next_resolve = fake_now + 500;      // main loop picks it up quickly
        }
        const uint8_t key = g64_track_key(akt_half_track);
        run_until(fake_now + 2000);
        const int from = nlog;
        run_until(fake_now + GCR_REVOLUTION_US + 50000);
        char what[64];
        snprintf(what, sizeof(what), "head at %.1f (plays %s %.1f)", akt_half_track / 2.0 + 1, (key & 1) ? "half-track" : "track", key / 2.0 + 1);
        CHECK(akt_half_track == pos, "head position follows each half step (%d)", akt_half_track);
        check_played_track(from, nlog, key, what);
    }

    // save as G64 and load again: whole and half-tracks must survive
    {
        static uint8_t saved_half[G64_TRACKCOUNT][G64_TRACKSIZE];
        static uint16_t saved_len[G64_TRACKCOUNT];
        for (int i = 0; i < G64_TRACKCOUNT; ++i)
        {
            saved_len[i] = g64_halftrack[i] ? g64_halftracklen[i] : 0;
            if (saved_len[i]) { memcpy(saved_half[i], g64_halftrack[i], saved_len[i]); }
        }
        static uint8_t saved_tracks[G64_TRACKCOUNT][G64_TRACKSIZE];
        static uint16_t saved_tracklen[G64_TRACKCOUNT];
        memcpy(saved_tracks, g64_tracks, sizeof(saved_tracks));
        memcpy(saved_tracklen, g64_tracklen, sizeof(saved_tracklen));

        const char* tmp = "drive_harness_save.g64";
        FILE* f = fopen(tmp, "wb");
        FIL fil; memset(&fil, 0, sizeof(fil)); fil.host_file = f;
        const int8_t w = write_disk(&fil, G64_IMAGE, num_max_tracks);
        fclose(f);
        CHECK(w >= 0, "write_disk G64 (result %d)", w);

        memset(g64_tracks, 0, sizeof(g64_tracks));
        const int8_t r = load_g64(tmp);
        int same = (r == last);
        for (int t = 0; t < num_max_tracks; ++t)
        {
            same &= (g64_tracklen[t] == saved_tracklen[t]) && (0 == memcmp(g64_tracks[t], saved_tracks[t], saved_tracklen[t]));
        }
        for (int i = 0; i < G64_TRACKCOUNT; ++i)
        {
            const uint16_t l = g64_halftrack[i] ? g64_halftracklen[i] : 0;
            same &= (l == saved_len[i]) && ((0 == l) || (0 == memcmp(g64_halftrack[i], saved_half[i], l)));
        }
        CHECK(same, "saved G64 loads back with identical tracks and half-tracks");
        remove(tmp);
    }
}

/////////////////////////////////////////////////////////////////////

int main(int argc, char** argv)
{
    scenario_playback();
    scenario_write();
    scenario_switch_in_sync();
    scenario_step();
    resolve_latency_us = 3000;
    scenario_two_writes();
    resolve_latency_us = 12000;
    scenario_two_writes();
    resolve_latency_us = 3000;
    scenario_bump();
    if (argc > 1)
    {
        scenario_halftracks(argv[1]);   // G64 with half-tracks, e.g. Bounty Bob Strikes Back
    }
    if (argc > 2)
    {
        scenario_density(argv[2]);      // RapidLok G64, e.g. Law of the West (PAL k2 s0)
    }
    printf(failures ? "\n%d FAILURES\n" : "\nALL OK\n", failures);
    return failures ? 1 : 0;
}
