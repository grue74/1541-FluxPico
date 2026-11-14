/**********************************
 * drive routines and control functions
 *
 * Author: F00K42
 * Last change: 2026/10/28
 ***********************************/

#include "main.h"
#include "drive.h"
#include "gcr_resolve.h"
#include "gcr.h"
#include "flux.h"

/////////////////////////////////////////////////////////////////////
// bit-level disk emulation
//
// The raw track (g64_tracks) is a bit-stream of 8*tracklen bitcells per
// revolution (300rpm -> 200ms). gcr_resolve() determines which SYNC and
// BYTE READY events the 1541 read circuit generates from it; the alarm
// callback below plays these events in real time. Time is a global
// "disk angle": raw bit q of every track passes the head at
//   rev * 200ms + q * (200ms / nbits)
// so the angular position is kept on track changes, like on a real disk.
/////////////////////////////////////////////////////////////////////

#define REV_Q16         (((uint64_t) GCR_REVOLUTION_US) << 16)  // revolution in us<<16
#define LATE_RESEEK_US  (1000)  // reposition if the callback is this late
#define POLL_BITS       (8)     // check OE/motor at least every byte time
#define BYTE_MIN_GAP_US (16)    // BYTE READY pulses never closer than this (drive loops need ~13us)

static gcr_resolved_t res_buf[2];                       // double buffer for track framing
static gcr_resolved_t* volatile res_active = NULL;      // framing used by the callback
static volatile uint32_t res_req_gen   = 0;             // incremented to request a resolve
static uint32_t          res_done_gen  = 0;             // last handled request
static volatile uint32_t res_image_gen = 0;             // incremented for every new image
static uint32_t          write_end_gen = 0;             // resolve request issued at the end of the last write

static alarm_id_t disk_alarm = 0;
static uint64_t alarm_target_us = 0;                   // time the disk alarm is set for
static uint64_t byte_pulse_us = 0;                     // time of the last BYTE READY pulse

typedef enum { PLAY_IDLE, PLAY_READ, PLAY_WRITE } play_mode_t;

// state of the alarm callback
static play_mode_t play_mode = PLAY_IDLE;
static const gcr_resolved_t* play_res = NULL;   // framing currently played
static gcr_cursor_t play_cur;                   // next read event
static uint32_t write_q;                        // raw bit position where the next written byte ends
static uint32_t write_rev;                      // revolution of write_q (raw coordinates)
static uint64_t bit_q16;                        // duration of a bitcell in us<<16
static uint64_t next_us;                        // time of the next callback
static uint64_t event_us;                       // time of the next read event (play_cur)
static uint64_t poll_us;                        // POLL_BITS bitcells in us
static uint32_t check_off;                      // position of the last read event/poll (offset ...
static uint32_t check_rev;                      // ... and revolution)
static uint32_t wr_first_q;                     // raw bit position of the first bit of the running write
static uint32_t wr_bits;                        // bits written by the running write
static uint32_t mute_q;                         // written area not yet in the framing: start ...
static uint32_t mute_len;                       // ... and length in bits
static uint8_t  hdr_gcr[5];                     // first GCR bytes after a sync (header check)
static uint8_t  hdr_count = 5;                  // bytes in hdr_gcr, 5 = not collecting

// drive activity for the display
volatile uint8_t  disk_header_sector = 0xFF;
volatile uint32_t disk_header_ms     = 0;
volatile uint8_t  disk_write_sector  = 0xFF;
volatile uint32_t disk_write_ms      = 0;
static uint32_t weak_rng = 0x1541BEEF;          // random source for weak bits

/////////////////////////////////////////////////////////////////////
// optional event log on USB serial (build with DEBUG_DRIVE_LOG).
// the disk emulation only stores small records in a ring buffer,
// drive_debug_service() in the main loop prints them.
/////////////////////////////////////////////////////////////////////

#ifdef DEBUG_DRIVE_LOG
enum { DL_HEAD = 1, DL_RESOLVED, DL_SYNC, DL_LATE, DL_WRITE_ON, DL_WRITE_OFF, DL_STEP_BAD, DL_IDLE, DL_REV, DL_MOTOR, DL_BLOCK, DL_SOE, DL_CAP };

typedef struct {
    uint32_t t_us;      // time of the event
    uint32_t a;         // event specific values
    uint32_t b;
    uint8_t  type;      // DL_*
    uint8_t  key;       // (half-)track key
    uint8_t  d[5];      // bytes after a sync
} dlog_t;

static dlog_t dlog[256];
static volatile uint8_t  dlog_w = 0;
static volatile uint8_t  dlog_r = 0;
static volatile uint32_t dlog_lost = 0;
static uint64_t dbg_step_us = 0;                // time of the last head move
static uint32_t dbg_bytes = 0;                  // BYTE READY pulses since the last sync
static uint32_t dbg_bytes_prev = 0;             // ... between the last two syncs
static uint32_t dbg_idle = 0;                   // idle (no framing yet) callbacks since the last head move
static volatile uint8_t dbg_sync_budget = 0;    // syncs still logged in detail after the last head move
static uint32_t dbg_rev = 0;                    // revolution the counters below belong to
static uint32_t dbg_rev_syncs = 0;              // syncs in that revolution
static uint32_t dbg_rev_bytes = 0;              // BYTE READY pulses in that revolution
static uint32_t dbg_last_syncs = 0xFFFFFFFF;    // last printed revolution summary ...
static uint32_t dbg_last_bytes = 0xFFFFFFFF;
static uint8_t  dbg_last_key = 0xFF;
static uint32_t dbg_same = 0;                   // ... and how often it repeated since
static uint32_t dbg_restarts = 0;               // playback restarts (skipped to the current angle) in this revolution
static int8_t   dbg_motor = -1;                 // last seen motor line, -1 = unknown
static int8_t   dbg_soe = -1;                   // last seen SOE line, -1 = unknown
static uint64_t dbg_soe_on_us = 0;              // SOE on since ...
static uint8_t  dbg_soe_on_sector = 0xFF;       // ... with this last header
static uint64_t dbg_job_end_us = 0;             // end of the last SOE period of 1ms or more
static uint8_t  dbg_cap_left = 0;               // header syncs still to capture
static bool     dbg_cap_active = false;         // capturing the bytes behind the current sync
static uint8_t  dbg_cap_n = 0;
static uint8_t  dbg_cap[13];                    // first 13 bytes after a header sync
static uint8_t  dbg_seg_first = 0;              // first byte after the current sync
static uint32_t dbg_blk_bytes = 0;              // BYTE READY pulses since the current sync

static void dlog_put(uint8_t type, uint8_t key, uint32_t a, uint32_t b, const uint8_t* d);

// a revolution of the read cursor is complete: log it if it differs from the last one
static void dbg_rev_done(const uint8_t key)
{
    if ((dbg_rev_syncs != dbg_last_syncs) || (dbg_rev_bytes != dbg_last_bytes) || (key != dbg_last_key) || dbg_restarts)
    {
        dlog_put(DL_REV, key, dbg_rev_syncs | (dbg_same << 16), dbg_rev_bytes | (dbg_restarts << 16), NULL);
        dbg_last_syncs = dbg_rev_syncs;
        dbg_last_bytes = dbg_rev_bytes;
        dbg_last_key   = key;
        dbg_same       = 0;
    } else {
        ++dbg_same;
    }
    dbg_rev_syncs = 0;
    dbg_restarts = 0;
    dbg_rev_bytes = 0;
}

static void dlog_put(uint8_t type, uint8_t key, uint32_t a, uint32_t b, const uint8_t* d)
{
    const uint32_t irq = save_and_disable_interrupts();
    const uint8_t w = dlog_w;
    if ((uint8_t) (w + 1) == dlog_r)
    {
        ++dlog_lost;
    } else {
        dlog_t* e = &dlog[w];
        e->t_us = (uint32_t) time_us_64();
        e->type = type;
        e->key  = key;
        e->a    = a;
        e->b    = b;
        if (d) { memcpy(e->d, d, 5); }
        dlog_w = (uint8_t) (w + 1);
    }
    restore_interrupts(irq);
}
#define DLOG(type, key, a, b, d)    dlog_put((type), (key), (a), (b), (d))
#else
#define DLOG(type, key, a, b, d)    do { } while (0)
#endif

// print the logged events (main loop)
void drive_debug_service(void)
{
#ifdef DEBUG_DRIVE_LOG
    static uint32_t lost_reported = 0;
    if (dlog_lost != lost_reported)
    {
        lost_reported = dlog_lost;
        printf("!! log overflow, %u events lost\n", (unsigned) lost_reported);
    }
    for (int n = 0; (n < 8) && (dlog_r != dlog_w); ++n)
    {
        const dlog_t* e = &dlog[dlog_r];
        const unsigned ms = (unsigned) (e->t_us / 1000);
        const unsigned tr = (e->key >> 1) + 1;
        const char*    hf = (e->key & 1) ? ".5" : "  ";
        switch (e->type)
        {
            case DL_HEAD:
                printf("%8u HEAD  pos %u%s  plays %u%s\n", ms, (unsigned) (e->a >> 1) + 1, (e->a & 1) ? ".5" : "", tr, (e->key & 1) ? ".5" : "");
                break;
            case DL_RESOLVED:
                printf("%8u READY %u%s  syncs %u  resolve %u us  since step %u us\n", ms, tr, hf, (unsigned) (e->a & 0xFFFF), (unsigned) (e->a >> 16), (unsigned) e->b);
                break;
            case DL_SYNC:
                printf("%8u SYNC  %u%s  at bit %5u  bytes since last sync %5u  next: %02X %02X %02X %02X %02X\n", ms, tr, hf, (unsigned) e->b, (unsigned) e->a, e->d[0], e->d[1], e->d[2], e->d[3], e->d[4]);
                break;
            case DL_LATE:
                printf("%8u LATE  %u%s  callback %u us late -> resynced by time\n", ms, tr, hf, (unsigned) e->a);
                break;
            case DL_WRITE_ON:
                printf("%8u WRITE %u%s  start at bit %u\n", ms, tr, hf, (unsigned) e->a);
                break;
            case DL_WRITE_OFF:
                printf("%8u WRITE %u%s  end, %u bits\n", ms, tr, hf, (unsigned) e->a);
                break;
            case DL_STEP_BAD:
                printf("%8u STEP? phase %u -> %u ignored (not one step)\n", ms, (unsigned) (e->a >> 2), (unsigned) (e->a & 3));
                break;
            case DL_REV:
                printf("%8u REV   %u%s  syncs %4u  bytes %5u  restarts %u   (previous summary repeated %u times)\n", ms, tr, hf, (unsigned) (e->a & 0xFFFF), (unsigned) (e->b & 0xFFFF), (unsigned) (e->b >> 16), (unsigned) (e->a >> 16));
                break;
            case DL_BLOCK:
                printf("%8u BLOCK %u%s  data block after header of sector %u read, %u bytes\n", ms, tr, hf, (unsigned) e->a, (unsigned) e->b);
                break;
            case DL_MOTOR:
                printf("%8u MOTOR %s\n", ms, e->a ? "on" : "off");
                break;
            case DL_CAP:
                printf("%8u HDR   %u%s  bytes 1-13 after header sync: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X  (byte 12 = %02X)\n", ms, tr, hf,
                       (unsigned) (e->a >> 24), (unsigned) ((e->a >> 16) & 0xFF), (unsigned) ((e->a >> 8) & 0xFF), (unsigned) (e->a & 0xFF),
                       (unsigned) (e->b >> 24), (unsigned) ((e->b >> 16) & 0xFF), (unsigned) ((e->b >> 8) & 0xFF), (unsigned) (e->b & 0xFF),
                       e->d[0], e->d[1], e->d[2], e->d[3], e->d[4], e->d[3]);
                break;
            case DL_SOE:
                printf("%8u SOE   on for %5u us until here, %u%s  (last header: sector %u at start, %u at end)\n", ms, (unsigned) e->a, tr, hf, (unsigned) (e->b & 0xFF), (unsigned) (e->b >> 8));
                break;
            case DL_IDLE:
                printf("%8u IDLE  %u%s  %u callbacks without data after the step\n", ms, tr, hf, (unsigned) e->a);
                break;
            default:
                break;
        }
        dlog_r = (uint8_t) (dlog_r + 1);
    }
#endif
}

/////////////////////////////////////////////////////////////////////

int64_t steppertimer_callback(alarm_id_t id, void *user_data)
{
    (void) id;
    (void) user_data;

    // stepping finished (the head position itself is updated with every
    // step in check_stepper_signals)
    turn_sound_off();

    return 0;
}

/////////////////////////////////////////////////////////////////////

void service_track_resolver(void)
{
#ifdef FLUX_MODE
    return;     // the gate array decodes the flux - no framing needed
#endif
    const uint32_t req = res_req_gen;
    if ((req == res_done_gen) || !is_image_mount)
    {
        return;     // nothing to do, or keep the request until an image is there
    }
    res_done_gen = req;

    const uint8_t  key   = g64_track_key(akt_half_track);   // whole or half-track under the head
    const uint32_t image = res_image_gen;
    uint16_t len;
    const uint8_t* const data = g64_track_data(key, &len);
    gcr_resolved_t* back = (res_active == &res_buf[0]) ? &res_buf[1] : &res_buf[0];

#ifdef DEBUG_RESOLVE_TIME
    const uint64_t t0 = time_us_64();
#endif
#ifdef DEBUG_DRIVE_LOG
    const uint64_t t_start = time_us_64();
#endif
    (void) gcr_resolve(data, len, back);
#ifdef DEBUG_RESOLVE_TIME
    printf("resolve track %u%s: %u syncs, %u us\n", (key >> 1) + 1, (key & 1) ? ".5" : "", back->nseg, (unsigned) (time_us_64() - t0));
#endif
    back->track     = key;
    back->image_gen = image;
    back->req_gen   = req;

    __compiler_memory_barrier();    // framing complete before the callback can see it
    res_active = back;      // the callback switches over at its next event

#ifdef DEBUG_DRIVE_LOG
    {
        const uint64_t now = time_us_64();
        DLOG(DL_RESOLVED, key, back->nseg | ((uint32_t) (now - t_start) << 16), (uint32_t) (now - dbg_step_us), NULL);
    }
#endif
}

/////////////////////////////////////////////////////////////////////

uint32_t mark_track_dirty(void)
{
    return ++res_req_gen;
}


void start_stepper_timer(void)
{
    turn_sound_on();

    if (stepper_alarm) { cancel_alarm(stepper_alarm); }
    stepper_alarm = add_alarm_in_ms(STEPPER_DELAY_TIME, steppertimer_callback, NULL, false);
}

/////////////////////////////////////////////////////////////////////

// the head is on a new (half-)track: the data there is needed right away -
// copy protections step half-tracks and expect the data at the same angle
static void head_moved(void)
{
    akt_track_pos  = 0;
    akt_half_track = selected_track;
    ++res_req_gen;
#ifdef DEBUG_DRIVE_LOG
    dbg_step_us = time_us_64();
    dbg_sync_budget = 2;
    DLOG(DL_HEAD, g64_track_key(selected_track), selected_track, 0, NULL);
#endif
}

// The stepper motor pulls the head to the position that belongs to the
// energized phase: phase = (position + 2) & 3 (position 0 = track 1, as in
// the 1541 and VICE: whole tracks have even phases, track 18 has phase 0).
// A new phase moves the head one half-track if a neighbour position has
// that phase; at the end stop (track 1) or with the opposite phase the head
// stays where it is - like the mechanics during a bump.
static inline uint8_t phase_of_position(const uint8_t pos)
{
    return (uint8_t) ((pos + 2) & 3);
}

void check_stepper_signals(void)
{
    if (stepper_signal_r_pos == stepper_signal_w_pos)
    {
        return;     // no new phase in the ring buffer
    }
    const uint8_t phase = stepper_signal_puffer[stepper_signal_r_pos] & 3;
    stepper_signal_r_pos++;

    if (phase == phase_of_position(selected_track))
    {
        return;     // head is already there
    }
    if (phase == phase_of_position((uint8_t) (selected_track + 1)))
    {
        if (selected_track < ((NUM_TRACKS_MAX - 1) << 1))
        {
            ++selected_track;
            head_moved();
        }
        start_stepper_timer();
    }
    else if (phase == phase_of_position((uint8_t) (selected_track + 3)))   // = position - 1
    {
        if (selected_track > 0)
        {
            --selected_track;
            head_moved();
        }
        start_stepper_timer();
    }
    else
    {
        // opposite phase: no force towards a neighbour, the head stays
        DLOG(DL_STEP_BAD, 0, (uint32_t) ((phase_of_position(selected_track) << 2) | phase), 0, NULL);
    }
}

/////////////////////////////////////////////////////////////////////

void send_disk_change(void)
{
    if(floppy_wp)
    {                   // WP enabled = wps set to 0
        set_wps();
        sleep_ms(1);
        clear_wps();
        sleep_ms(1);
        set_wps();
        sleep_ms(1);
        clear_wps();
    } else {            // WP disabled = wps set to 1
        clear_wps();
        sleep_ms(1);
        set_wps();
        sleep_ms(1);
        clear_wps();
        sleep_ms(1);
        set_wps();
    }
}

/////////////////////////////////////////////////////////////////////

void init_writeprot(void)
{
    gpio_init(GPIO_WPS);
    gpio_set_dir(GPIO_WPS, GPIO_IN);
    gpio_set_pulls(GPIO_WPS, false, false);
    // remark:
    // 1541 schematics include a 74ls04 which drives WPS low by default
    // having a pull-up enabled is not a good idea
}

/////////////////////////////////////////////////////////////////////

void unmount_image(void)
{
    close_disk_image(&fd);
    is_image_mount = 0;
    akt_image_type = UNDEF_IMAGE;
    num_max_tracks = 0;
    enable_write_protection();
    menu_set_entry_var1(&image_menu, M_WP_IMAGE, 1);
    send_disk_change();
}

/////////////////////////////////////////////////////////////////////

void show_sdcard_info_message(void)
{
    sd_card_t* my_sdcard;
    uint32_t sdcard_size = 0;

    if (0 != fs.fs_type)    // check for valid mounted file-system
    {
        display_clear();
        display_home();

        display_string(disp_sdinfo_size_s);
        my_sdcard = sd_get_by_num(0);   // get a sd_card_info-struct from current card
        if (NULL != my_sdcard)
        {
            sdcard_size = my_sdcard->get_num_sectors(my_sdcard);
            char data[9];
            dez2out((int32_t)(sdcard_size*512E-9),0,data);
            display_string(data);
        }
        display_string(" GB");

        display_setcursor(0,1);
        display_string(disp_sdinfo_part_s);

        switch (fs.fs_type)
        {
            case FS_FAT12:
                display_string("FAT12");
                break;
            case FS_FAT16:
                display_string("FAT16");
                break;
            case FS_FAT32:
                display_string("FAT32");
                break;
            case FS_EXFAT:
                display_string("EXT FAT");
                break;
            default:
                break;
        }
    }
    //sleep_ms(START_MESSAGE_TIME);

    // struct sd_raw_info info;

    // uint8_t counter = 5;
    // uint8_t get_info_ok = 0;
    // char out_str[21];

    // while(counter != 0)
    // {
    //     if(0 != sd_raw_get_info(&info))
    //     {
    //         display_setcursor(disp_sdinfo_manuf_p);
    //         display_string(disp_sdinfo_manuf_s);
    //         sprintf(out_str, "%.x", info.manufacturer);
    //         display_string(out_str);

    //         display_setcursor(disp_sdinfo_oem_p);
    //         display_string(disp_sdinfo_oem_s);
    //         display_string((char*) info.oem);

    //         display_setcursor(disp_sdinfo_prod_p);
    //         display_string(disp_sdinfo_prod_s);
    //         display_string((char*) info.product);

    //         display_setcursor(disp_sdinfo_size_p);
    //         display_string(disp_sdinfo_size_s);
    //         sprintf(out_str, "%d MB", (uint16_t)(info.capacity / 1024 / 1024));
    //         display_string(out_str);

    //         get_info_ok = 1;

    //         break;
    //     }

    //     release_sd_card();
    //     (void) init_sd_card();

    //     counter--;
    // }

    // if(!get_info_ok)
    // {
    //     display_clear();
    //     display_setcursor(disp_geterr_failure_p);
    //     display_string(disp_geterr_failure_s);
    //     display_setcursor(disp_sdrawgetinfo_p);
    //     display_string(disp_sdrawgetinfo_s);
    //     return;
    // }

    // _delay_ms(START_MESSAGE_TIME);

    // display_clear();

    // display_setcursor(disp_sdinfo_rev_p);
    // display_string(disp_sdinfo_rev_s);
    // sprintf(out_str,"%c.%c",(info.revision>>4)+'0', (info.revision&0x0f)+'0');
    // display_string(out_str);

    // display_setcursor(disp_sdinfo_serial_p);
    // display_string(disp_sdinfo_serial_s);
    // sprintf(out_str,"%04X%04X",(unsigned int) (info.serial >> 16),(unsigned int) (info.serial & 0xffff));
    // display_string(out_str);

    // display_setcursor(disp_sdinfo_part_p);
    // display_string(disp_sdinfo_part_s);
    // switch(partition->type)
    // {
    //     case 0x01:
    //         display_string("FAT12");
    //         break;

    //     case 0x04:
    //         display_string("FAT16 <32MB");
    //         break;

    //     case 0x05:
    //         display_string("EXTENDED");
    //         break;

    //     case 0x06:
    //         display_string("FAT16");
    //         break;

    //     case 0x0b:
    //         display_string("FAT32");
    //         break;

    //     case 0x0c:
    //         display_string("FAT32 LBA");
    //         break;

    //     case 0x0e:
    //         display_string("FAT16 LBA");
    //         break;

    //     case 0x0f:
    //         display_string("EXT LBA");
    //         break;

    //     default:
    //         display_string("UNKNOWN");
    //         break;
    // }
    // _delay_ms(START_MESSAGE_TIME);
}

/////////////////////////////////////////////////////////////////////

void init_stepper(void)
{
    // Stepper PINs als Eingang schalten
    gpio_init(GPIO_STP0);
    gpio_init(GPIO_STP1);
    gpio_set_pulls(GPIO_STP0, true, false);
    gpio_set_pulls(GPIO_STP1, true, false);
    gpio_set_dir(GPIO_STP0, GPIO_IN);
    gpio_set_dir(GPIO_STP1, GPIO_IN);

    selected_track = (INIT_TRACK << 1);
    akt_half_track = selected_track;

    // Pin Change Interrupt für beide STPx PIN's aktivieren
    gpio_set_irq_enabled(GPIO_STP0, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
    gpio_set_irq_enabled(GPIO_STP1, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
}

/////////////////////////////////////////////////////////////////////

void stepper_inc(void)
{
    if(selected_track >= ((NUM_TRACKS_MAX-1)<<1)) return;

    ++selected_track;
}

/////////////////////////////////////////////////////////////////////

void stepper_dec(void)
{
    if(selected_track == 0) return;

    --selected_track;
}

/////////////////////////////////////////////////////////////////////

// put the head on (or next to) a whole track without a step from the drive,
// e.g. INIT_TRACK (track 18) at power up and for the selector menu. track is
// 0-based like INIT_TRACK (0 = track 1). The position must belong to the
// stepper phase the drive outputs (phase_of_position): the nearest one to
// the track - the track itself, half a track or a whole track beside it.
void set_head_near_track(const uint8_t track)
{
    const int32_t target = (int32_t) track << 1;
    const int32_t want   = ((int32_t) current_stepper_phase() - 2) & 3;  // position mod 4 for this phase
    int32_t pos = target + ((want - target) & 3);                         // target .. target+3
    if (pos > target + 1)
    {
        pos -= 4;                                                       // target-2 .. target+1
    }
    if (pos < 0)
    {
        pos += 4;
    }
    if (pos > ((NUM_TRACKS_MAX - 1) << 1))
    {
        pos -= 4;
    }
    selected_track = (uint8_t) pos;
    akt_half_track = selected_track;
}

/////////////////////////////////////////////////////////////////////

void init_motor(void)
{
    // Als Eingang schalten
    gpio_init(GPIO_MTR);
    gpio_set_pulls(GPIO_MTR, true, false);
    gpio_set_dir(GPIO_MTR, GPIO_IN);
}

/////////////////////////////////////////////////////////////////////

void init_control_signals(void)
{
    // Als Ausgang schalten
    //DDxn = 0 , PORTxn = 0 --> HiZ
    //DDxn = 1 , PORTxn = 0 --> Output Low (Sink)
    gpio_init(GPIO_BRDY);
    gpio_set_pulls(GPIO_BRDY, false, false);
    gpio_set_dir(GPIO_BRDY, GPIO_IN);
    // BYTE_READY_DDR &= ~(1<<BYTE_READY);             // Byte Ready auf HiZ
    // BYTE_READY_PORT &= ~(1<<BYTE_READY);

    gpio_init(GPIO_SYNC);
#ifdef FLUX_MODE
    // the gate array drives SYNC (and BYTE READY and port A): inputs only
    gpio_set_dir(GPIO_SYNC, GPIO_IN);
#else
    gpio_set_dir(GPIO_SYNC, GPIO_OUT);

    gpio_set_drive_strength(GPIO_SYNC, GPIO_DRIVE_STRENGTH_12MA);
#endif

    // Als Eingang schalten
    gpio_init_mask(PAPORT_MASK);
    gpio_set_pulls(GPIO_PAPORT  , true, false);
    gpio_set_pulls(GPIO_PAPORT+1, true, false);
    gpio_set_pulls(GPIO_PAPORT+2, true, false);
    gpio_set_pulls(GPIO_PAPORT+3, true, false);
    gpio_set_pulls(GPIO_PAPORT+4, true, false);
    gpio_set_pulls(GPIO_PAPORT+5, true, false);
    gpio_set_pulls(GPIO_PAPORT+6, true, false);
    gpio_set_pulls(GPIO_PAPORT+7, true, false);
    gpio_set_dir_in_masked(PAPORT_MASK); // should set all 8 bits to input

#ifndef FLUX_MODE
    gpio_set_drive_strength(GPIO_PAPORT  , GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(GPIO_PAPORT+1, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(GPIO_PAPORT+2, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(GPIO_PAPORT+3, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(GPIO_PAPORT+4, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(GPIO_PAPORT+5, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(GPIO_PAPORT+6, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(GPIO_PAPORT+7, GPIO_DRIVE_STRENGTH_12MA);
#endif

    gpio_init(GPIO_SOE);
    gpio_set_pulls(GPIO_SOE, true, false);
    gpio_set_dir(GPIO_SOE, GPIO_IN);

    gpio_init(GPIO_OE);
    gpio_set_pulls(GPIO_OE, true, false);
    gpio_set_dir(GPIO_OE, GPIO_IN);
}

/////////////////////////////////////////////////////////////////////

#ifndef FLUX_MODE     // FLUX_MODE: GPIO3 is the flux output (flux.c)
void init_soe_gatearray(void)
{
    gpio_init(GPIO_SOE_GA);
    gpio_set_dir(GPIO_SOE_GA, GPIO_OUT);
    set_soe_gatearray();
}
#endif

/////////////////////////////////////////////////////////////////////

void init_bytetimer(void)
{
    stop_bytetimer();
}

/////////////////////////////////////////////////////////////////////

// (re)start the disk rotation after a new image was put into memory
void start_bytetimer(uint8_t half_track)
{
    (void) half_track;
#ifdef FLUX_MODE
    flux_image_start();
#else
    stop_bytetimer();

    ++res_image_gen;            // framing of the old image is invalid now
    ++res_req_gen;              // resolve the current track

    play_mode = PLAY_IDLE;
    next_us   = time_us_64() + 100;
    alarm_target_us = next_us;
    disk_alarm = add_alarm_at(from_us_since_boot(next_us), disk_alarm_callback, NULL, true);
#endif
}

/////////////////////////////////////////////////////////////////////

void stop_bytetimer(void)
{
#ifdef FLUX_MODE
    flux_image_stop();
#else
    if (disk_alarm > 0)
    {
        (void) cancel_alarm(disk_alarm);
    }
    disk_alarm = 0;
#endif
}

/////////////////////////////////////////////////////////////////////

void init_sound(void)
{
    gpio_init(GPIO_SND);
    gpio_set_dir(GPIO_SND, GPIO_OUT);
    turn_sound_off();
}

///////////////////////////////////////
///////// ISR
///////////////////////////////////////

// the byte-level playback below drives SYNC, BYTE READY and port A. In
// FLUX_MODE the gate array drives them - this code must not exist there.
#ifndef FLUX_MODE

static inline uint64_t read_event_us(void)
{
    return ((uint64_t) play_cur.rev * REV_Q16
            + (uint64_t) (play_res->origin + play_cur.off) * bit_q16) >> 16;
}

static inline uint64_t write_event_us(void)
{
    return ((uint64_t) write_rev * REV_Q16 + (uint64_t) write_q * bit_q16) >> 16;
}

// next callback: the next read event, but poll at least every POLL_BITS
// bitcells (inside a sync there can be no event for a long time)
static inline void schedule_read(void)
{
    event_us = read_event_us();
    const uint64_t poll = next_us + poll_us;
    next_us = (event_us < poll) ? event_us : poll;
}

/////////////////////////////////////////////////////////////////////

// offset (relative to origin of play_res) of the bit under the head at t_us
static uint32_t offset_at(const uint64_t t_us, uint32_t* rev_out)
{
    const uint64_t t     = t_us << 16;
    int64_t  rev         = (int64_t) (t / REV_Q16);
    uint32_t q           = (uint32_t) ((t - (uint64_t) rev * REV_Q16) / bit_q16);
    if (q >= play_res->nbits) { q = play_res->nbits - 1; }

    int32_t off = (int32_t) q - (int32_t) play_res->origin;
    if (off < 0)
    {
        off += (int32_t) play_res->nbits;
        --rev;
    }
    *rev_out = (uint32_t) rev;
    return (uint32_t) off;
}

// position the read cursor behind offset off of revolution rev
static void play_seek_off(const uint32_t off, const uint32_t rev)
{
    play_cur.rev = rev;
    if (gcr_cursor_seek(play_res, &play_cur, off))
    {
        clear_sync();
    } else {
        set_sync();
    }
}

/////////////////////////////////////////////////////////////////////

static inline void pulse_byte_ready(void)
{
    if (send_byte_ready)
    {
        // never two pulses closer than BYTE_MIN_GAP_US: when the interrupt
        // was late and catches up, the drive's BVC loop would see one byte.
        // (only the pulse waits - the playback itself stays on time)
        const uint64_t now = time_us_64();
        if (now < (byte_pulse_us + BYTE_MIN_GAP_US))
        {
            busy_wait_us(byte_pulse_us + BYTE_MIN_GAP_US - now);
        }
        byte_pulse_us = time_us_64();
        // BYTE_READY für 3µs löschen
        clear_byte_ready();
        busy_wait_us(3);
        set_byte_ready();
    }
}

/////////////////////////////////////////////////////////////////////

// no valid track data: motor off, no image, or framing still being resolved
static void play_idle(const bool pending)
{
    play_mode = PLAY_IDLE;
    set_sync();

    if (get_so_status())
    {
        // read mode: like before - motor off sends 0x00 bytes
        if (!pending && get_soe_status())
        {
            gpio_set_dir_out_masked(PAPORT_MASK);
            out_gcr_byte(0x00);
            pulse_byte_ready();
        }
    } else {
        // write mode: accept bytes, but there is nothing to write them to
        if (!block_data_changes && get_soe_status())
        {
            gpio_set_dir_in_masked(PAPORT_MASK);
            pulse_byte_ready();
        }
    }
    next_us += bytetimer_values[d64_track_zone[akt_half_track>>1]];
}

/////////////////////////////////////////////////////////////////////

// first 5 GCR bytes after a sync: a sector header (08, checksum, sector, track)?
static void check_header(const uint64_t now)
{
    if (0x52 != hdr_gcr[0])
    {
        return;                     // GCR of 0x08 starts with 0x52 - not a header
    }
    uint8_t header[4];
    ConvertFromGCR(hdr_gcr, header);
    if (0x08 == header[0])
    {
        disk_header_sector = header[2];
        disk_header_ms     = (uint32_t) (now / 1000);
    }
}

/////////////////////////////////////////////////////////////////////

// a write ended: remember the written area and request a new framing
static void finish_write(const gcr_resolved_t* res)
{
    const bool earlier_pending = (int32_t) (res->req_gen - write_end_gen) < 0;
    if (earlier_pending || (wr_bits >= res->nbits))
    {
        mute_q   = 0;               // several writes or a whole track: all of it
        mute_len = res->nbits;
    } else {
        mute_q   = wr_first_q;
        mute_len = wr_bits;
    }
    write_end_gen = mark_track_dirty();     // the main loop resolves the track again
}

// true if raw position q is in written data the framing does not know yet
static inline bool is_muted(const gcr_resolved_t* res, const uint32_t q)
{
    if ((int32_t) (res->req_gen - write_end_gen) >= 0)
    {
        return false;               // framing is up to date
    }
    const uint32_t d = (q >= mute_q) ? (q - mute_q) : (q + res->nbits - mute_q);
    return d < mute_len;
}

/////////////////////////////////////////////////////////////////////

// handle the event (or poll) due at next_us and set next_us to the following one
static void disk_process(void)
{
    const gcr_resolved_t* res = res_active;
    const uint8_t key = g64_track_key(akt_half_track);    // whole or half-track under the head

#ifdef DEBUG_DRIVE_LOG
    {
        const int8_t m = get_motor_status() ? 1 : 0;
        if (m != dbg_motor)
        {
            dbg_motor = m;
            DLOG(DL_MOTOR, key, (uint32_t) m, 0, NULL);
        }
        const int8_t e = get_soe_status() ? 1 : 0;
        if (e != dbg_soe)
        {
            // DOS switches SOE on for a disk job and off at its end. The
            // job loop also polls every 15ms with ~0.1ms SOE pulses - only
            // on-periods of 1ms or more are logged.
            const uint64_t t = time_us_64();
            if (e)
            {
                dbg_soe_on_us     = t;
                dbg_soe_on_sector = disk_header_sector;
                if ((t - dbg_job_end_us) < 2000)
                {
                    // SOE on right after a job (like a loader started by B-E):
                    // capture what the next header syncs deliver
                    dbg_cap_left = 3;
                }
            } else if ((dbg_soe > 0) && ((t - dbg_soe_on_us) >= 1000)) {
                DLOG(DL_SOE, key, (uint32_t) (t - dbg_soe_on_us), dbg_soe_on_sector | ((uint32_t) disk_header_sector << 8), NULL);
                dbg_job_end_us = t;
            }
            dbg_soe = e;
        }
    }
#endif
    if (!(get_motor_status() && is_image_mount))
    {
        play_idle(false);
        return;
    }
    if ((NULL == res) || (res->image_gen != res_image_gen) || (res->track != key) || (0 == res->nbits))
    {
        play_idle(true);    // head moved and new framing not yet available, or empty track
#ifdef DEBUG_DRIVE_LOG
        ++dbg_idle;
#endif
        return;
    }

    const uint64_t now = time_us_64();
    const bool late = (now > (next_us + LATE_RESEEK_US));
#ifdef DEBUG_DRIVE_LOG
    if (dbg_idle)
    {
        DLOG(DL_IDLE, key, dbg_idle, 0, NULL);
        dbg_idle = 0;
    }
    if (late && (PLAY_IDLE != play_mode))
    {
        DLOG(DL_LATE, key, (uint32_t) (now - next_us), 0, NULL);
    }
#endif

    // a new framing is taken over in read mode only - a running write
    // keeps its bit position (it does not depend on the framing)
    if ((PLAY_IDLE == play_mode) || late || ((res != play_res) && (PLAY_WRITE != play_mode)))
    {
        if (PLAY_WRITE == play_mode)
        {
            finish_write(res);      // written data needs a new framing
        }
        // (re)start playback at the current disk angle
#ifdef DEBUG_DRIVE_LOG
        ++dbg_restarts;
#endif
        play_res = res;
        bit_q16  = REV_Q16 / res->nbits;
        poll_us  = (POLL_BITS * bit_q16) >> 16;
        uint32_t rev;
        const uint32_t off = offset_at(now, &rev);
        play_seek_off(off, rev);
        check_off = off;
        check_rev = rev;
        play_mode = PLAY_READ;
        next_us = now;
        schedule_read();
        return;
    }

    uint16_t raw_len;
    uint8_t* const raw = g64_track_data(key, &raw_len);
    const uint32_t nbits = res->nbits;

    if (get_so_status())
    {
        // LESE MODUS
        if (PLAY_WRITE == play_mode)
        {
            // write finished: continue reading behind the written data
            DLOG(DL_WRITE_OFF, key, wr_bits, 0, NULL);
            finish_write(res);
            play_mode = PLAY_READ;
            play_res  = res;

            int32_t off = (int32_t) write_q - (int32_t) res->origin;
            uint32_t rev = write_rev;
            if (off < 0)
            {
                off += (int32_t) nbits;
                --rev;
            }
            play_seek_off((uint32_t) off, rev);
            check_off = (uint32_t) off;
            check_rev = rev;
            schedule_read();
            return;
        }

        if (next_us < event_us)
        {
            // only a poll, nothing changed (the position by time can be a
            // bit early - never go back behind the last event)
            uint32_t rev;
            const uint32_t off = offset_at(next_us, &rev);
            if (((uint64_t) rev * nbits + off) > ((uint64_t) check_rev * nbits + check_off))
            {
                check_off = off;
                check_rev = rev;
            }
            schedule_read();
            return;
        }

        // events inside data written since the last resolve are muted (the
        // framing there may be wrong) - the cursor keeps running anyway
        const bool mute = is_muted(res, gcr_raw_pos(res, play_cur.off));

        switch (play_cur.type)
        {
            case GCR_EV_SYNC_ON:
                if (!mute)
                {
                    clear_sync();           // SYNC Leitung auf Low setzen
#ifdef DEBUG_DRIVE_LOG
                    if ((0x55 == dbg_seg_first) && (dbg_blk_bytes >= 32))
                    {
                        // a data block was read with BYTE READY: sector of the header before it
                        DLOG(DL_BLOCK, key, disk_header_sector, dbg_blk_bytes, NULL);
                    }
                    dbg_seg_first = 0;
#endif
                    // the read shift register is full of 1-bits during a sync
                    gpio_set_dir_out_masked(PAPORT_MASK);
                    out_gcr_byte(0xFF);
                }
                break;

            case GCR_EV_SYNC_OFF:
                set_sync();                 // SYNC Leitung auf High setzen
                hdr_count = 0;              // look for a sector header behind the sync
#ifdef DEBUG_DRIVE_LOG
                dbg_bytes_prev = dbg_bytes;
                dbg_bytes = 0;
                dbg_blk_bytes = 0;
                dbg_cap_n = 0;
                dbg_cap_active = (dbg_cap_left > 0);
                ++dbg_rev_syncs;
#endif
                break;

            default:
                if (!mute)
                {
                    // like the 1541: the read shift register always feeds the
                    // VIA port, SOE only gates BYTE READY. Some loaders poll
                    // $1C01 with SOE off (RapidLok). The bus transceiver
                    // direction follows the read/write mode line, not SOE.
                    const uint8_t akt_gcr_byte = gcr_fetch_byte(raw, nbits, gcr_raw_pos(res, play_cur.off), &weak_rng, (uint8_t) (3 - d64_track_zone[key >> 1]));
                    gpio_set_dir_out_masked(PAPORT_MASK);
                    out_gcr_byte(akt_gcr_byte);
#ifdef DEBUG_DRIVE_LOG
                    if (dbg_cap_active)
                    {
                        dbg_cap[dbg_cap_n++] = akt_gcr_byte;
                        if ((1 == dbg_cap_n) && (0x52 != akt_gcr_byte))
                        {
                            dbg_cap_active = false;     // not a sector header
                        } else if (13 == dbg_cap_n) {
                            DLOG(DL_CAP, key,
                                 ((uint32_t) dbg_cap[0] << 24) | ((uint32_t) dbg_cap[1] << 16) | ((uint32_t) dbg_cap[2] << 8) | dbg_cap[3],
                                 ((uint32_t) dbg_cap[4] << 24) | ((uint32_t) dbg_cap[5] << 16) | ((uint32_t) dbg_cap[6] << 8) | dbg_cap[7],
                                 &dbg_cap[8]);
                            dbg_cap_active = false;
                            --dbg_cap_left;
                        }
                    }
#endif
                    if (get_soe_status())
                    {
                        pulse_byte_ready();
#ifdef DEBUG_DRIVE_LOG
                        ++dbg_bytes;
                        ++dbg_rev_bytes;
                        ++dbg_blk_bytes;
#endif
                    }
                    {
                        if (hdr_count < 5)
                        {
#ifdef DEBUG_DRIVE_LOG
                            if (0 == hdr_count) { dbg_seg_first = akt_gcr_byte; }
#endif
                            hdr_gcr[hdr_count++] = akt_gcr_byte;
                            if (5 == hdr_count)
                            {
                                check_header(now);
#ifdef DEBUG_DRIVE_LOG
                                if (dbg_sync_budget)
                                {
                                    --dbg_sync_budget;
                                    DLOG(DL_SYNC, key, dbg_bytes_prev, play_cur.off, hdr_gcr);
                                }
#endif
                            }
                        }
                    }
                }
                break;
        }
        check_off = play_cur.off;
        check_rev = play_cur.rev;
        gcr_cursor_next(res, &play_cur);
#ifdef DEBUG_DRIVE_LOG
        if (play_cur.rev != dbg_rev)
        {
            dbg_rev = play_cur.rev;
            dbg_rev_done(key);
        }
#endif
        schedule_read();
    }
    else
    {
        // SCHREIB MODUS
        if (PLAY_WRITE != play_mode)
        {
            // switch from read: the bit counter keeps the read framing,
            // the byte currently being read is the first one overwritten
            uint32_t rev = play_cur.rev;
            uint32_t off = play_cur.off;            // at an event: its exact position
            if (next_us < event_us)
            {
                // at a poll: OE changed since the last check - the bit after
                // that position is the earliest the switch can have happened
                // (a byte completed at the last check was already read)
                off = check_off + 1;
                rev = check_rev;
                if (off >= nbits)
                {
                    off -= nbits;
                    ++rev;
                }
            }
            const uint32_t boundary = gcr_write_boundary(res, off);

            play_mode = PLAY_WRITE;
            write_q   = res->origin + boundary;
            write_rev = rev;
            while (write_q >= nbits)
            {
                write_q -= nbits;
                ++write_rev;
            }
            wr_first_q = (write_q >= 7) ? (write_q - 7) : (write_q + nbits - 7);
            wr_bits    = 0;
            DLOG(DL_WRITE_ON, key, boundary, 0, NULL);
            set_sync();
            next_us = write_event_us();     // the callback loop calls again if it is due now
            return;
        }

        if (!block_data_changes)    // only accept changes when no image-dump is happening
        {
            if (get_soe_status())
            {
                gpio_set_dir_in_masked(PAPORT_MASK);
                const uint8_t akt_gcr_byte = in_gcr_byte();
                pulse_byte_ready();

                if (!track_is_written)
                {
                    track_write_nr  = key >> 1;
                    track_write_pos = (uint16_t) (write_q >> 3);
                }
                gcr_write_byte(raw, nbits, write_q, akt_gcr_byte);
                track_is_written = true;
                disk_write_sector = disk_header_sector;     // written behind the last header
                disk_write_ms     = (uint32_t) (now / 1000);
            }
        }

        wr_bits += 8;
        write_q += 8;
        if (write_q >= nbits)
        {
            write_q -= nbits;
            ++write_rev;
        }
        next_us = write_event_us();
    }
}

/////////////////////////////////////////////////////////////////////

int64_t disk_alarm_callback(alarm_id_t id, void *user_data)
{
    (void) id;
    (void) user_data;

    const uint64_t scheduled = alarm_target_us;     // the time this alarm was set for

    // process all events that are due (events can be closer than 1us)
    uint32_t guard = 0;
    do
    {
        disk_process();
    } while ((next_us <= scheduled) && (++guard < 16));

    uint64_t target = next_us;
    if (target <= scheduled)
    {
        target = scheduled + 1;
    }
    alarm_target_us = target;
    // negative: relative to the time this alarm was scheduled for -> no drift
    return -(int64_t) (target - scheduled);
}

#endif  // !FLUX_MODE
