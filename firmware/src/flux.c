/**********************************
 * flux.c - flux stream into the drive's own read circuit (FLUX_MODE)
 *
 * Read path: the raw track bytes (g64_tracks / half-track pool) are sent
 * by DMA to a PIO state machine that spends one bitcell per bit and puts
 * a pulse on GPIO3 for every 1-bit. GPIO3 is wired to the gate array's
 * CLR input, where the read amplifier's pulses arrived before. The gate
 * array decodes them at the density the drive selects and drives SYNC,
 * BYTE READY and port A itself.
 *
 * - a revolution takes 200 ms: the PIO clock divider follows the track
 *   length, the disk angle is global (kept on head steps)
 * - two DMA channels chained in a circle play a track endlessly; the
 *   DMA interrupt re-arms the finished one once per revolution
 * - pulses are only sent in read mode (MODE high, jmp pin in the PIO):
 *   a real head sends none while writing, and every pulse would reset
 *   the gate array's bit counter
 *
 * Write path: a second state machine takes a snapshot of GPIO1..16
 * (MODE, SYNC, port A) at every falling BYTE READY edge; DMA puts them
 * into cap_ring. In write mode these are the bytes the drive writes; the
 * GPIO interrupt on the first BYTE READY of a write gives its time, and
 * so its bit position on the track.
 *
 * Calibration: the pulse polarity and width the gate array needs are not
 * known (no measurement of the read amplifier). At start the Pico plays a
 * test pattern with several shapes and watches the gate array's SYNC.
 ***********************************/

#ifdef FLUX_MODE

#include <stdio.h>
#include <string.h>

#include <stdarg.h>

#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/sync.h"

#include "main.h"
#include "drive.h"
#include "globals.h"
#include "gcr.h"
#include "gcr_resolve.h"
#include "flux.h"
#include "flux_math.h"

#define FLUX_PIN        GPIO_SOE_GA     // GPIO3 -> wire C -> gate array CLR (pin 31)
#define MODE_PIN        GPIO_OE         // GPIO1 <- VIA CB2 through Q6: high = read
#define BRDY_PIN        GPIO_BRDY       // GPIO22 <- BYTE READY of the gate array
#define CAP_BASE        (1u)            // capture snapshot: GPIO1 .. GPIO16
#define CAP_COUNT       (16u)
#define CAP_MODE_BIT    (1u << (GPIO_OE - CAP_BASE))
#define CAP_SYNC_BIT    (1u << (GPIO_SYNC - CAP_BASE))
#define CAP_PA_SHIFT    (GPIO_PAPORT - CAP_BASE)
#define CAP_RING_LEN    (1024u)         // snapshots (uint16): about 26 ms of bytes
#define CAP_RING_MASK   (CAP_RING_LEN - 1u)
#define CAP_RING_BITS   (11u)           // log2 of the ring size in bytes (DMA ring)

#define CAL_CELL_NS     (3250u)         // calibration pattern: bitcell of zone 3
#define CAL_SETTLE_US   (1000u)
#define CAL_MEASURE_US  (6000u)
#define WRITE_ANCHOR_WAIT_US (2000u)    // longest wait for the write start interrupt

static const PIO fpio = pio1;
static uint sm_flux, sm_cap;
static uint off_flux, off_cap;
static int  dma_a = -1, dma_b = -1, dma_cap = -1;
static uint32_t clk_hz;

static uint16_t cap_ring[CAP_RING_LEN] __attribute__((aligned(CAP_RING_LEN * 2)));
static uint32_t cap_rd = 0;             // next snapshot to look at

// pulse shape (from the calibration)
static bool     pulse_active_low = true;
static uint32_t pulse_ns         = 400;
volatile bool   flux_ok          = false;

// what is played: track data for the DMA interrupt and the angle model.
// The bit under the head at time t is p0 + bits(t - t0) (mod nbits).
static const uint8_t* volatile cur_data = NULL;
static volatile uint32_t cur_len = 0;
static volatile bool streaming = false;
typedef struct {
    uint64_t t0;        // time the first bit of this segment was played
    uint32_t p0;        // bit position of that bit in the track
    uint32_t nbits;
    uint32_t div;       // PIO clock divider (8.8)
    uint32_t w;         // pulse delay field
    uint8_t  key;       // track key (g64_track_key), 0xFF: calibration pattern
    uint32_t gen;       // image generation
} flux_seg_t;
static flux_seg_t seg = { .key = 0xFF };

// image changes (files.c on core1) - acted on by the main loop on core0
static volatile uint32_t image_gen    = 0;
static volatile bool     image_paused = true;

// write start: time and capture index of a BYTE READY edge in write mode
static volatile bool     anchor_valid = false;
static volatile uint64_t anchor_t     = 0;
static volatile uint32_t anchor_idx   = 0;
static uint64_t          wr_wait_since = 0;

// sector header search in the bytes the gate array reads
static uint8_t hdr_win[5];

// statistics for the USB log
static uint32_t st_read = 0, st_hdr = 0, st_wr = 0, st_wr_lost = 0, st_restart = 0;
static uint64_t st_next_us = 0;
static int8_t   last_motor = -1;

// the calibration runs before a USB terminal is connected: keep its lines
// and print them again when one connects
static char   cal_log[1600];
static size_t cal_log_len = 0;
static bool   cal_log_shown = false;

static void cal_printf(const char* fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0)
    {
        return;
    }
    fputs(line, stdout);
    const size_t len = strlen(line);
    if ((cal_log_len + len) < sizeof(cal_log))
    {
        memcpy(cal_log + cal_log_len, line, len + 1);
        cal_log_len += len;
    }
}

/////////////////////////////////////////////////////////////////////
// PIO programs

// one bitcell per pass, W + A + B + 6 = FLUX_CELL_CYCLES on every path:
//  0: out x, 1          next bit of the track (autopull, one byte per FIFO word)
//  1: jmp !x, 4         0: no flux reversal
//  2: jmp pin, 6        1 in read mode (MODE high): pulse
//  3: jmp 7 [W]         1 in write mode: no pulse
//  4: nop [W]           no flux reversal
//  5: jmp 7
//  6: set pins, 1 [W]   the pulse, W+1 cycles (the pad may be inverted)
//  7: set pins, 0 [A]
//  8: nop [B]
// base: 0 for pio_add_program (it relocates jumps), off_flux for a live patch
static void flux_encode(uint16_t* p, const uint base, const uint32_t w)
{
    uint32_t a, b;
    flux_tail_delays(w, &a, &b);
    p[0] = (uint16_t) pio_encode_out(pio_x, 1);
    p[1] = (uint16_t) pio_encode_jmp_not_x(base + 4);
    p[2] = (uint16_t) pio_encode_jmp_pin(base + 6);
    p[3] = (uint16_t) (pio_encode_jmp(base + 7) | pio_encode_delay(w));
    p[4] = (uint16_t) (pio_encode_nop() | pio_encode_delay(w));
    p[5] = (uint16_t) pio_encode_jmp(base + 7);
    p[6] = (uint16_t) (pio_encode_set(pio_pins, 1) | pio_encode_delay(w));
    p[7] = (uint16_t) (pio_encode_set(pio_pins, 0) | pio_encode_delay(a));
    p[8] = (uint16_t) (pio_encode_nop() | pio_encode_delay(b));
}

// bitcell length and pulse width for a divider (the pulse keeps its width in ns)
static void flux_set_timing(const uint32_t div)
{
    uint16_t p[9];
    const uint32_t w = flux_width_field(clk_hz, div, pulse_ns);
    flux_encode(p, off_flux, w);
    pio_sm_set_clkdiv_int_frac8(fpio, sm_flux, div >> 8, (uint8_t) (div & 0xFF));
    for (uint i = 0; i < 9; ++i)
    {
        fpio->instr_mem[off_flux + i] = p[i];
    }
    seg.div = div;
    seg.w   = w;
}

// GPIO3 to the PIO with the chosen polarity; inactive level first
static void flux_pin_take(void)
{
    pio_sm_set_enabled(fpio, sm_flux, false);
    gpio_set_outover(FLUX_PIN, pulse_active_low ? GPIO_OVERRIDE_INVERT : GPIO_OVERRIDE_NORMAL);
    pio_sm_set_pins_with_mask(fpio, sm_flux, 0, 1u << FLUX_PIN);
    pio_sm_set_pindirs_with_mask(fpio, sm_flux, 1u << FLUX_PIN, 1u << FLUX_PIN);
    pio_gpio_init(fpio, FLUX_PIN);
    pio_sm_set_enabled(fpio, sm_flux, true);
}

// no working pulse shape: GPIO3 low, as the old firmware held SOE_GA
static void flux_pin_release(void)
{
    pio_sm_set_enabled(fpio, sm_flux, false);
    gpio_set_outover(FLUX_PIN, GPIO_OVERRIDE_NORMAL);
    gpio_init(FLUX_PIN);
    gpio_put(FLUX_PIN, false);
    gpio_set_dir(FLUX_PIN, GPIO_OUT);
}

/////////////////////////////////////////////////////////////////////
// DMA: two channels chained in a circle

static void dma_arm(const int ch, const int chain_to, const uint8_t* src, const uint32_t count, const bool go)
{
    dma_channel_config c = dma_channel_get_default_config((uint) ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);     // a byte write fills the whole FIFO word
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(fpio, sm_flux, true));
    channel_config_set_chain_to(&c, (uint) chain_to);
    dma_channel_configure((uint) ch, &c, &fpio->txf[sm_flux], src, count, go);
}

// once per revolution: the channel that finished plays the whole track next time
static void flux_dma_irq(void)
{
    const uint32_t mask = (1u << dma_a) | (1u << dma_b);
    const uint32_t st = dma_hw->ints1 & mask;
    if (0 == st)
    {
        return;
    }
    dma_hw->ints1 = st;
    if (!streaming)
    {
        return;
    }
    if (st & (1u << dma_a))
    {
        dma_channel_set_read_addr((uint) dma_a, cur_data, false);
        dma_channel_set_trans_count((uint) dma_a, cur_len, false);
    }
    if (st & (1u << dma_b))
    {
        dma_channel_set_read_addr((uint) dma_b, cur_data, false);
        dma_channel_set_trans_count((uint) dma_b, cur_len, false);
    }
}

static void stream_stop(void)
{
    const uint32_t irq = save_and_disable_interrupts();
    streaming = false;
    // chain to itself = no chain, so aborting one cannot start the other
    hw_write_masked(&dma_hw->ch[dma_a].al1_ctrl, (uint32_t) dma_a << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB, DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS);
    hw_write_masked(&dma_hw->ch[dma_b].al1_ctrl, (uint32_t) dma_b << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB, DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS);
    dma_channel_abort((uint) dma_a);
    dma_channel_abort((uint) dma_b);
    dma_hw->ints1 = (1u << dma_a) | (1u << dma_b);
    restore_interrupts(irq);
    // the state machine plays the rest of its byte and then waits at
    // "out" - the pin stays at its inactive level (no flux)
    pio_sm_clear_fifos(fpio, sm_flux);
}

// play len bytes endlessly, starting at byte off
static void stream_play(const uint8_t key, const uint8_t* data, const uint32_t len, const uint32_t off, const uint32_t div)
{
    stream_stop();
    flux_set_timing(div);
    cur_data = data;
    cur_len  = len;
    dma_arm(dma_b, dma_a, data, len, false);
    dma_arm(dma_a, dma_b, data + off, len - off, false);
    seg.key   = key;
    seg.nbits = len * 8u;
    seg.p0    = off * 8u;
    seg.gen   = image_gen;
    streaming = true;
    seg.t0 = time_us_64();
    dma_channel_start((uint) dma_a);
    ++st_restart;
}

// bit position under the head at time t (current segment)
static uint32_t seg_bit_at(const uint64_t t)
{
    const uint64_t dt = (t > seg.t0) ? (t - seg.t0) : 0;
    const uint64_t bits = flux_bits_in_us(clk_hz, seg.div, dt);
    return (uint32_t) ((seg.p0 + (bits % seg.nbits)) % seg.nbits);
}

/////////////////////////////////////////////////////////////////////

void flux_init(void)
{
    clk_hz = clock_get_hz(clk_sys);

    // flux output state machine
    sm_flux = (uint) pio_claim_unused_sm(fpio, true);
    uint16_t p[9];
    flux_encode(p, 0, 7);
    const pio_program_t prog = { .instructions = p, .length = 9, .origin = -1 };
    off_flux = (uint) pio_add_program(fpio, &prog);
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, off_flux, off_flux + 8);
    sm_config_set_set_pins(&c, FLUX_PIN, 1);
    sm_config_set_jmp_pin(&c, MODE_PIN);
    sm_config_set_out_shift(&c, false, true, 8);    // MSB first, a byte per FIFO word
    pio_sm_init(fpio, sm_flux, off_flux, &c);
    flux_set_timing(flux_div_q8_for_cell(clk_hz, CAL_CELL_NS));

    // capture state machine: snapshot at every falling BYTE READY edge
    sm_cap = (uint) pio_claim_unused_sm(fpio, true);
    uint16_t cp[3];
    cp[0] = (uint16_t) pio_encode_wait_gpio(false, BRDY_PIN);
    cp[1] = (uint16_t) (pio_encode_in(pio_pins, CAP_COUNT) | pio_encode_delay(15));  // ~0.1us, port A settles
    cp[2] = (uint16_t) pio_encode_wait_gpio(true, BRDY_PIN);
    const pio_program_t cprog = { .instructions = cp, .length = 3, .origin = -1 };
    off_cap = (uint) pio_add_program(fpio, &cprog);
    pio_sm_config cc = pio_get_default_sm_config();
    sm_config_set_wrap(&cc, off_cap, off_cap + 2);
    sm_config_set_in_pins(&cc, CAP_BASE);
    sm_config_set_in_shift(&cc, false, true, CAP_COUNT);
    sm_config_set_fifo_join(&cc, PIO_FIFO_JOIN_RX);
    pio_sm_init(fpio, sm_cap, off_cap, &cc);

    dma_cap = dma_claim_unused_channel(true);
    dma_channel_config d = dma_channel_get_default_config((uint) dma_cap);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_16);
    channel_config_set_read_increment(&d, false);
    channel_config_set_write_increment(&d, true);
    channel_config_set_ring(&d, true, CAP_RING_BITS);
    channel_config_set_dreq(&d, pio_get_dreq(fpio, sm_cap, false));
    dma_channel_configure((uint) dma_cap, &d, cap_ring, &fpio->rxf[sm_cap], dma_encode_endless_transfer_count(), true);
    pio_sm_set_enabled(fpio, sm_cap, true);

    // track playback channels
    dma_a = dma_claim_unused_channel(true);
    dma_b = dma_claim_unused_channel(true);
    dma_channel_set_irq1_enabled((uint) dma_a, true);
    dma_channel_set_irq1_enabled((uint) dma_b, true);
    irq_add_shared_handler(DMA_IRQ_1, flux_dma_irq, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_1, true);

    // write start: falling MODE edge arms the BYTE READY interrupt
    gpio_set_irq_enabled(MODE_PIN, GPIO_IRQ_EDGE_FALL, true);

    flux_pin_release();     // until the calibration has chosen a pulse shape
}

/////////////////////////////////////////////////////////////////////

static inline uint32_t cap_wr_index(void)
{
    return (uint32_t) (((uintptr_t) dma_hw->ch[dma_cap].write_addr - (uintptr_t) cap_ring) / 2u) & CAP_RING_MASK;
}

bool flux_gpio_irq(uint gpio, uint32_t events)
{
    if (MODE_PIN == gpio)
    {
        if (events & GPIO_IRQ_EDGE_FALL)
        {
            // write mode begins: the next BYTE READY gives the position
            anchor_valid = false;
            gpio_set_irq_enabled(BRDY_PIN, GPIO_IRQ_EDGE_FALL, true);
        }
        return true;
    }
    if (BRDY_PIN == gpio)
    {
        const uint64_t t = time_us_64();
        const uint32_t w = cap_wr_index();      // the PIO took this edge's snapshot ~0.1us after it
        gpio_set_irq_enabled(BRDY_PIN, GPIO_IRQ_EDGE_FALL, false);
        if (!gpio_get(MODE_PIN))
        {
            anchor_t     = t;
            anchor_idx   = (w - 1u) & CAP_RING_MASK;
            anchor_valid = true;
        }
        return true;
    }
    return false;
}

/////////////////////////////////////////////////////////////////////
// calibration

static uint8_t cal_pattern[64];         // 8 x: 16 one-bits (a sync), then 0x55 x 6

static void cal_measure(uint32_t* edges, uint32_t* low_permille)
{
    sleep_us(CAL_SETTLE_US);
    const uint64_t end = time_us_64() + CAL_MEASURE_US;
    uint32_t n = 0, low = 0, e = 0;
    bool last = true;
    while (time_us_64() < end)
    {
        const bool s = gpio_get(GPIO_SYNC);     // low = SYNC
        ++n;
        if (!s)
        {
            ++low;
            if (last)
            {
                ++e;
            }
        }
        last = s;
    }
    *edges = e;
    *low_permille = n ? (uint32_t) ((uint64_t) low * 1000u / n) : 0;
}

bool flux_calibrate(void)
{
    static const uint16_t widths_ns[] = { 100, 150, 200, 300, 400, 500, 700, 900, 1200, 1500 };
    enum { NW = sizeof(widths_ns) / sizeof(widths_ns[0]) };
    bool pass[2][NW];
    uint8_t npass[2] = { 0, 0 };

    for (uint i = 0; i < sizeof(cal_pattern); ++i)
    {
        cal_pattern[i] = ((i & 7) < 2) ? 0xFF : 0x55;
    }
    // expected: one SYNC per 64 bitcells, low for about 7 of them
    const uint32_t expect = CAL_MEASURE_US * 1000u / (64u * CAL_CELL_NS);

    if (!gpio_get(MODE_PIN))
    {
        cal_printf("flux cal: MODE is low (write mode) - is wire A / Q6 right? using defaults\n");
    }
    const uint32_t div = flux_div_q8_for_cell(clk_hz, CAL_CELL_NS);
    cal_printf("flux cal: clk %lu Hz, div %lu.%03lu, expect %lu syncs per %u us\n",
           (unsigned long) clk_hz, (unsigned long) (div >> 8), (unsigned long) ((div & 0xFF) * 1000u / 256u),
           (unsigned long) expect, CAL_MEASURE_US);

    for (uint pol = 0; pol < 2; ++pol)
    {
        pulse_active_low = (0 == pol);
        flux_pin_take();
        for (uint i = 0; i < NW; ++i)
        {
            pulse_ns = widths_ns[i];
            stream_play(0xFF, cal_pattern, sizeof(cal_pattern), 0, div);
            uint32_t edges, low;
            cal_measure(&edges, &low);
            stream_stop();
            pass[pol][i] = (edges * 4u >= expect * 3u) && (edges * 4u <= expect * 5u) && (low >= 40u) && (low <= 250u);
            if (pass[pol][i])
            {
                ++npass[pol];
            }
            cal_printf("flux cal: %s %4u ns (W=%2lu): %3lu syncs, SYNC low %3lu/1000 %s\n",
                   pulse_active_low ? "active-low " : "active-high", widths_ns[i], (unsigned long) seg.w,
                   (unsigned long) edges, (unsigned long) low, pass[pol][i] ? "OK" : "-");
        }
    }

    const uint best = (npass[1] > npass[0]) ? 1u : 0u;
    if (0 == npass[best])
    {
        cal_printf("flux cal: no pulse shape works - no flux output (mod done? see doc/flux-mod-1541-II.pdf)\n");
        flux_pin_release();
        flux_ok = false;
        return false;
    }
    // middle of the widths that work
    uint idx[NW];
    uint n = 0;
    for (uint i = 0; i < NW; ++i)
    {
        if (pass[best][i])
        {
            idx[n++] = i;
        }
    }
    pulse_active_low = (0 == best);
    pulse_ns = widths_ns[idx[n / 2]];
    flux_pin_take();
    cal_printf("flux cal: using %s pulses of %lu ns (%u of %u widths work)\n",
           pulse_active_low ? "active-low" : "active-high", (unsigned long) pulse_ns, n, NW);
    flux_ok = true;
    seg.key = 0xFF;
    return true;
}

/////////////////////////////////////////////////////////////////////
// image changes (called from files.c on core1)

void flux_image_stop(void)
{
    image_paused = true;
}

void flux_image_start(void)
{
    ++image_gen;
    image_paused = false;
}

/////////////////////////////////////////////////////////////////////
// bytes taken at BYTE READY

// a sector header (52 ...) in the last 5 read bytes: sector for the display
static void header_check(const uint8_t byte, const uint64_t now)
{
    memmove(hdr_win, hdr_win + 1, 4);
    hdr_win[4] = byte;
    if (0x52 != hdr_win[0])
    {
        return;
    }
    uint8_t h[4];
    ConvertFromGCR(hdr_win, h);
    if ((0x08 == h[0]) && (0xFF != seg.key) && (h[3] == (uint8_t) ((seg.key >> 1) + 1)) && (h[2] < 21))
    {
        disk_header_sector = h[2];
        disk_header_ms     = (uint32_t) (now / 1000u);
        ++st_hdr;
    }
}

// a byte the drive writes: into the track at its bit position
static void write_byte_at(const uint32_t idx, const uint8_t byte, const uint64_t now)
{
    if (!streaming || (0xFF == seg.key) || block_data_changes || (anchor_t < seg.t0))
    {
        ++st_wr_lost;
        return;
    }
    int32_t d = (int32_t) ((idx - anchor_idx) & CAP_RING_MASK);
    if (d >= (int32_t) (CAP_RING_LEN / 2u))
    {
        d -= (int32_t) CAP_RING_LEN;
    }
    const uint32_t nbits = seg.nbits;
    int64_t pos = (int64_t) seg_bit_at(anchor_t) + 8 * (int64_t) d;
    pos %= (int64_t) nbits;
    if (pos < 0)
    {
        pos += nbits;
    }
    uint16_t len;
    uint8_t* raw = g64_track_data(seg.key, &len);
    if ((NULL == raw) || ((uint32_t) len * 8u != nbits))
    {
        ++st_wr_lost;
        return;
    }
    gcr_write_byte(raw, nbits, (uint32_t) ((pos + 7) % nbits), byte);
    if (!track_is_written)
    {
        track_write_nr  = seg.key >> 1;
        track_write_pos = (uint16_t) (pos >> 3);
    }
    track_is_written  = true;
    disk_write_sector = disk_header_sector;
    disk_write_ms     = (uint32_t) (now / 1000u);
    ++st_wr;
}

static void process_captures(const uint64_t now)
{
    const uint32_t w = cap_wr_index();
    while (cap_rd != w)
    {
        const uint16_t v = cap_ring[cap_rd];
        const uint8_t byte = (uint8_t) (v >> CAP_PA_SHIFT);
        if (v & CAP_MODE_BIT)
        {
            ++st_read;
            header_check(byte, now);
        } else {
            if (!anchor_valid)
            {
                // the write start interrupt comes right after the edge
                if (0 == wr_wait_since)
                {
                    wr_wait_since = now;
                }
                if ((now - wr_wait_since) < WRITE_ANCHOR_WAIT_US)
                {
                    return;
                }
                ++st_wr_lost;
            } else {
                write_byte_at(cap_rd, byte, now);
            }
            wr_wait_since = 0;
        }
        cap_rd = (cap_rd + 1u) & CAP_RING_MASK;
    }
}

/////////////////////////////////////////////////////////////////////

void flux_service(void)
{
    const uint64_t now = time_us_64();

    if (flux_ok)
    {
        const bool motor = get_motor_status();
        if ((int8_t) motor != last_motor)
        {
            last_motor = (int8_t) motor;
            printf("flux: motor %s\n", motor ? "on" : "off");
        }
        const bool want = !image_paused && is_image_mount && motor;
        if (!want)
        {
            if (streaming)
            {
                stream_stop();
            }
        } else {
            const uint8_t key = g64_track_key(akt_half_track);
            if (!streaming || (key != seg.key) || (image_gen != seg.gen))
            {
                uint16_t len;
                const uint8_t* data = g64_track_data(key, &len);
                if ((NULL == data) || (len < 16u))
                {
                    if (streaming)
                    {
                        stream_stop();
                    }
                } else {
                    const uint32_t off = flux_angle_byte(now, len);
                    stream_play(key, data, len, off, flux_div_q8(clk_hz, (uint32_t) len * 8u));
                    printf("flux: track %u%s, %u bytes, byte %lu, div %lu.%03lu, W %lu\n",
                           (key >> 1) + 1, (key & 1) ? ".5" : "", len, (unsigned long) off,
                           (unsigned long) (seg.div >> 8), (unsigned long) ((seg.div & 0xFF) * 1000u / 256u),
                           (unsigned long) seg.w);
                }
            }
        }
    }
    process_captures(now);
}

void flux_debug_service(void)
{
    const uint64_t now = time_us_64();
    if (!cal_log_shown && stdio_usb_connected())
    {
        cal_log_shown = true;
        printf("\n1541-FluxPico flux mode. Calibration at start:\n%s", cal_log);
    }
    if (now < st_next_us)
    {
        return;
    }
    st_next_us = now + 2000000u;
    if (st_read || st_wr || st_wr_lost)
    {
        printf("flux: 2s: %lu bytes read, %lu headers, %lu written, %lu lost, %lu restarts\n",
               (unsigned long) st_read, (unsigned long) st_hdr, (unsigned long) st_wr,
               (unsigned long) st_wr_lost, (unsigned long) st_restart);
    }
    st_read = st_hdr = st_wr = st_wr_lost = st_restart = 0;
}

#endif
