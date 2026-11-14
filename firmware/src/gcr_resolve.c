/**********************************
 * bit-level resolution of raw GCR tracks (G64)
 *
 * see gcr_resolve.h for the overview.
 *
 * read circuit model (VICE rotation.c, 1541 long board naming):
 *  - every flux reversal (1-bit in the image) resets UE7/UF4, a 1 is
 *    shifted into UD2 half a bitcell later
 *  - without flux reversal UF4 wraps after 4 bitcells and shifts in a
 *    1 by itself: "1 0000" is read as "1 0001"
 *  - after ~18us without flux the analog part produces random flux
 *    reversals -> bits are random ("weak bits")
 *  - the last 10 bits of the shifter being 1 asserts SYNC and keeps the
 *    bit counter UE3 cleared; the first 0 ends SYNC and is bit 1 of the
 *    next byte; every 8th bit afterwards gives BYTE READY
 *
 * Last change: 2026/11/12
 ***********************************/

#include "gcr_resolve.h"

// number of trailing zero bits, x != 0
#if defined(__GNUC__)
#define count_trailing_zeros(x) ((uint32_t) __builtin_ctz(x))
#else
static inline uint32_t count_trailing_zeros(uint32_t x)
{
    uint32_t n = 0;
    while (!(x & 1)) { x >>= 1; ++n; }
    return n;
}
#endif

static inline uint32_t next_pos(uint32_t q, uint32_t nbits)
{
    return (++q >= nbits) ? 0 : q;
}

static inline uint32_t raw_bit(const uint8_t* raw, uint32_t q)
{
    return (raw[q >> 3] >> (7 - (q & 7))) & 1;
}

/////////////////////////////////////////////////////////////////////

// find a raw bit position q with bit(q)==1 and bit(q-1)==0 (circular).
// returns nbits if there is no such position (track is all 0 or all 1)
static uint32_t find_rising_edge(const uint8_t* raw, uint32_t len_bytes)
{
    uint32_t prev = raw[len_bytes - 1];
    for (uint32_t i = 0; i < len_bytes; ++i)
    {
        const uint32_t cur = raw[i];
        // for each bit: previous bit in time is the next higher bit
        const uint32_t rising = cur & ~(((prev << 8) | cur) >> 1) & 0xFF;
        if (rising)
        {
            uint32_t bit = 7;
            while (!(rising & (1u << bit))) { --bit; }
            return (i << 3) + (7 - bit);
        }
        prev = cur;
    }
    return len_bytes << 3;
}

/////////////////////////////////////////////////////////////////////

// decoded bit for a raw bit, k = number of zeros since last flux reversal
static inline uint32_t decode_bit(uint32_t raw_b, uint32_t* k)
{
    if (raw_b)
    {
        *k = 0;
        return 1;
    }
    ++(*k);
    // UF4 wraps every 4 bitcells without flux reversal and shifts in a 1
    // (beyond k=4 the bits are random on real hw; use the wrap pattern)
    return ((*k & 3) == 0) ? 1 : 0;
}

/////////////////////////////////////////////////////////////////////

// decode a whole raw byte (same result as 8x decode_bit).
// returns false if the byte needs bit-by-bit decoding.
static inline bool decode_byte(uint32_t byte, uint32_t* k, uint32_t* decoded)
{
    if (0 == byte)
    {
        // no flux at all: only the UF4 wrap pattern, phase given by k
        static const uint8_t wrap_pattern[4] = { 0x11, 0x22, 0x44, 0x88 };
        *decoded = wrap_pattern[*k & 3];
        *k += 8;
        return true;
    }
    // a run of 4+ zeros (counting the zeros before this byte) inside the byte
    const uint32_t z = (~byte & 0xFF) | (((1u << ((*k > 3) ? 3 : *k)) - 1) << 8);
    if (z & (z >> 1) & (z >> 2) & (z >> 3))
    {
        return false;
    }
    *decoded = byte;
    *k = count_trailing_zeros(byte);
    return true;
}

// true if 10 one-bits end inside the new byte (shift = last 10 bits before it)
static inline bool sync_in_byte(uint32_t shift, uint32_t decoded)
{
    const uint32_t w  = (shift << 8) | decoded;
    const uint32_t r2 = w & (w >> 1);
    const uint32_t r4 = r2 & (r2 >> 2);
    const uint32_t r8 = r4 & (r4 >> 4);
    return 0 != ((r8 & (r8 >> 2)) & 0xFF);
}

/////////////////////////////////////////////////////////////////////

// simulate one revolution starting at origin (state: SYNC active)
// returns the segment index of a sync still active at the end of the
// revolution, or -1 if none
static int32_t resolve_from(const uint8_t* raw, gcr_resolved_t* out)
{
    const uint32_t nbits = out->nbits;
    uint32_t q = out->origin;
    uint32_t shift = 0x3FF;
    uint32_t k = 0;
    bool in_sync = true;        // sync active in the shifter
    bool tracked = true;        // the active sync is stored as a segment

    out->nseg = 1;
    out->overflow = false;
    out->seg[0].sync_on  = 0;
    out->seg[0].sync_off = GCR_RES_NO_OFFSET;

    if (++q >= nbits) { q = 0; }

    for (uint32_t off = 1; off < nbits; ++off, q = next_pos(q, nbits))
    {
        // fast path: a whole raw byte at once, when no sync starts or
        // ends inside it (most of a track)
        while ((0 == (q & 7)) && ((off + 8) <= nbits))
        {
            const uint32_t byte = raw[q >> 3];
            if (in_sync)
            {
                if (0xFF != byte) { break; }    // sync may end here
                // stays in sync: shifter keeps 0x3FF, k stays 0
            } else {
                uint32_t k_new = k;
                uint32_t decoded;
                if (!decode_byte(byte, &k_new, &decoded) || sync_in_byte(shift, decoded)) { break; }
                shift = ((shift << 8) | decoded) & 0x3FF;
                k = k_new;
            }
            off += 8;
            q   += 8;
            if (q >= nbits) { q = 0; }
        }
        if (off >= nbits) { break; }

        shift = ((shift << 1) | decode_bit(raw_bit(raw, q), &k)) & 0x3FF;

        if (0x3FF == shift)
        {
            if (!in_sync)
            {
                in_sync = true;
                if (out->nseg < GCR_RES_MAX_SEGMENTS)
                {
                    out->seg[out->nseg].sync_on  = (uint16_t) off;
                    out->seg[out->nseg].sync_off = GCR_RES_NO_OFFSET;
                    ++out->nseg;
                    tracked = true;
                } else {
                    out->overflow = true;   // ignore this sync, bytes continue
                    tracked = false;
                }
            }
        } else if (in_sync) {
            in_sync = false;
            if (tracked)
            {
                out->seg[out->nseg - 1].sync_off = (uint16_t) off;
            }
        }
    }

    return (in_sync && tracked) ? (int32_t) (out->nseg - 1) : -1;
}

/////////////////////////////////////////////////////////////////////

// scan from a rising edge (the bit before it is a 0) for the first sync.
// returns the raw position of the bit completing 10 one-bits, or nbits
static uint32_t find_first_sync(const uint8_t* raw, uint32_t nbits, uint32_t edge)
{
    uint32_t q = edge;
    uint32_t shift = 0;
    uint32_t k = 0;

    for (uint32_t n = 0; n < nbits + 10; ++n, q = next_pos(q, nbits))
    {
        // fast path: whole bytes without sync
        while ((0 == (q & 7)) && ((n + 8) <= (nbits + 10)))
        {
            uint32_t k_new = k;
            uint32_t decoded;
            if (!decode_byte(raw[q >> 3], &k_new, &decoded) || sync_in_byte(shift, decoded)) { break; }
            shift = ((shift << 8) | decoded) & 0x3FF;
            k = k_new;
            n += 8;
            q += 8;
            if (q >= nbits) { q = 0; }
        }
        if (n >= nbits + 10) { break; }

        shift = ((shift << 1) | decode_bit(raw_bit(raw, q), &k)) & 0x3FF;
        if (0x3FF == shift)
        {
            return q;
        }
    }
    return nbits;
}

/////////////////////////////////////////////////////////////////////

uint16_t gcr_resolve(const uint8_t* raw, uint32_t len_bytes, gcr_resolved_t* out)
{
    const uint32_t nbits = len_bytes << 3;

    out->nbits    = nbits;
    out->origin   = 0;
    out->nseg     = 0;
    out->overflow = false;

    if (0 == len_bytes)
    {
        return 0;
    }

    const uint32_t edge = find_rising_edge(raw, len_bytes);
    if (edge >= nbits)
    {
        if (raw[0] & 0x80)
        {
            // only 1-bits: SYNC all the time, never a byte
            out->nseg = 1;
            out->seg[0].sync_on  = 0;
            out->seg[0].sync_off = GCR_RES_NO_OFFSET;
        }
        // else only 0-bits: no sync, random data
        return out->nseg;
    }

    // the bit before the edge is a 0, so a sync found from here starts here
    const uint32_t first_sync = find_first_sync(raw, nbits, edge);
    if (first_sync >= nbits)
    {
        // track without sync: framing is arbitrary, use byte boundaries
        return 0;
    }
    out->origin = first_sync;

    // steady state: the state right after the sync bit is known exactly
    int32_t open_seg = resolve_from(raw, out);

    if (open_seg > 0)
    {
        // the sync of the last segment runs over the end of the revolution
        // into segment 0 - so it really starts there. Restart from it.
        out->origin = gcr_raw_pos(out, out->seg[open_seg].sync_on);
        (void) resolve_from(raw, out);
    }

    return out->nseg;
}

/////////////

static inline uint32_t seg_limit(const gcr_resolved_t* res, uint32_t s)
{
    return ((s + 1) < res->nseg) ? res->seg[s + 1].sync_on : res->nbits;
}

// cursor is at segment s, the next byte would end at offset b
static void cursor_byte_or_next(const gcr_resolved_t* res, gcr_cursor_t* cur, uint32_t b)
{
    if (0 == res->nseg)
    {
        if (b >= res->nbits)
        {
            b -= res->nbits;
            ++cur->rev;
        }
        cur->off  = b;
        cur->type = GCR_EV_BYTE;
        return;
    }

    if (b < seg_limit(res, cur->seg))
    {
        cur->off  = b;
        cur->type = GCR_EV_BYTE;
        return;
    }

    // continue with the sync of the next segment
    if (++cur->seg >= res->nseg)
    {
        cur->seg = 0;
        ++cur->rev;
    }
    cur->off  = res->seg[cur->seg].sync_on;
    cur->type = GCR_EV_SYNC_ON;
}

/////////////////////////////////////////////////////////////////////

void gcr_cursor_next(const gcr_resolved_t* res, gcr_cursor_t* cur)
{
    switch (cur->type)
    {
        case GCR_EV_SYNC_ON:
        {
            const uint32_t sync_off = res->seg[cur->seg].sync_off;
            if (GCR_RES_NO_OFFSET != sync_off)
            {
                cur->off  = sync_off;
                cur->type = GCR_EV_SYNC_OFF;
            } else {
                // sync without end (until next segment)
                cursor_byte_or_next(res, cur, res->nbits);
            }
        }
        break;

        case GCR_EV_SYNC_OFF:
            // sync_off is the first bit of the byte -> complete after 8 bits
            cursor_byte_or_next(res, cur, cur->off + 7);
            break;

        default:
            cursor_byte_or_next(res, cur, cur->off + 8);
            break;
    }
}

/////////////////////////////////////////////////////////////////////

bool gcr_cursor_seek(const gcr_resolved_t* res, gcr_cursor_t* cur, uint32_t off)
{
    if (0 == res->nseg)
    {
        // bytes end at offsets 7, 15, 23, ...
        cur->seg = 0;
        cursor_byte_or_next(res, cur, (off & ~7u) + 7 + ((7 == (off & 7)) ? 8 : 0));
        return false;
    }

    // binary search: last segment with sync_on <= off (seg[0].sync_on == 0)
    uint32_t lo = 0;
    uint32_t hi = res->nseg - 1;
    while (lo < hi)
    {
        const uint32_t mid = (lo + hi + 1) >> 1;
        if (res->seg[mid].sync_on <= off) { lo = mid; } else { hi = mid - 1; }
    }
    cur->seg = (uint16_t) lo;

    const uint32_t sync_off = res->seg[lo].sync_off;
    if ((GCR_RES_NO_OFFSET == sync_off) || (off < sync_off))
    {
        // inside the sync phase
        cur->type = GCR_EV_SYNC_ON;     // pretend we just processed SYNC_ON
        cur->off  = res->seg[lo].sync_on;
        gcr_cursor_next(res, cur);
        return true;
    }

    // inside the byte phase: bytes end at sync_off + 7 + 8n
    const uint32_t first = sync_off + 7;
    uint32_t b = first;
    if (off >= first)
    {
        b = first + (((off - first) >> 3) + 1) * 8;
    }
    cursor_byte_or_next(res, cur, b);
    return false;
}

/////////////////////////////////////////////////////////////////////

uint32_t gcr_write_boundary(const gcr_resolved_t* res, uint32_t off)
{
    uint32_t base;  // a known byte end of the framing at or before off+8

    if (0 == res->nseg)
    {
        base = 7;
    } else {
        uint32_t lo = 0;
        uint32_t hi = res->nseg - 1;
        while (lo < hi)
        {
            const uint32_t mid = (lo + hi + 1) >> 1;
            if (res->seg[mid].sync_on <= off) { lo = mid; } else { hi = mid - 1; }
        }
        const uint32_t sync_off = res->seg[lo].sync_off;
        if ((GCR_RES_NO_OFFSET == sync_off) || (off < sync_off))
        {
            return off + 8;     // inside a sync: counter restarts after this bit
        }
        base = sync_off + 7;
    }

    if (off <= base)
    {
        return base;
    }
    return base + (((off - base) + 7) & ~7u);
}

/////////////////////////////////////////////////////////////////////

static inline uint32_t xorshift32(uint32_t* s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

// probability (of 256) that the k-th bit after the last flux reversal reads
// as 1 when no further flux follows, k = 5..11+ (k = 1..3: 0, k = 4: 1).
// Measured with the VICE read circuit model (rotation_1541_gcr): the first
// random flux reversal comes 289..319 clocks (18-20us) after the last real
// one, so the first bits of a gap are almost fixed - RapidLok header bytes
// end in such gaps and must read the same every time.
static const uint16_t gap_p[4][7] = {
    { 256,  44,  51,  61, 148,  97,  97 },     // density 0 (tracks 31-35)
    { 256,  36,  49,  56, 154,  92,  95 },     // density 1
    { 169, 110,  41,  49, 125, 108,  92 },     // density 2
    {   0, 256,  31,  41,  46, 169,  87 },     // density 3 (tracks 1-17)
};

uint8_t gcr_fetch_byte(const uint8_t* raw, uint32_t nbits, uint32_t q, uint32_t* rng, uint8_t density)
{
    uint32_t v;     // bit 0 = raw bit q, bit n = raw bit q-n (17 valid bits)

    if (q >= 16)
    {
        const uint32_t i = q >> 3;
        v = ((uint32_t) raw[i - 2] << 16) | ((uint32_t) raw[i - 1] << 8) | raw[i];
        v >>= (7 - (q & 7));
    } else {
        // wrap around the end of the track
        v = 0;
        uint32_t p = q + nbits - 16;
        while (p >= nbits) { p -= nbits; }
        for (int n = 16; n >= 0; --n)
        {
            v = (v << 1) | raw_bit(raw, p);
            if (++p >= nbits) { p = 0; }
        }
    }

    uint32_t data = v & 0xFF;

    if (0 == (v & 0x1FFFF))
    {
        // inside a long gap without flux (17 zeros or more): all 8 bits random
        const uint32_t p = gap_p[density & 3][6];
        uint32_t r = 0;
        for (uint32_t n = 0; n < 8; ++n, r >>= 8)
        {
            if (0 == (n & 3)) { r = xorshift32(rng); }     // 4 random bytes per call
            if ((r & 0xFF) < p) { data |= 1u << n; }
        }
        return (uint8_t) data;
    }

    // runs of 4 or more 0-bits ending in this byte need special treatment
    const uint32_t z = ~v & 0x1FFFF;
    if (z & (z >> 1) & (z >> 2) & (z >> 3) & 0xFF)
    {
        for (uint32_t n = 0; n < 8; ++n)
        {
            if (!(z & (1u << n))) { continue; }
            // count zeros: this one and the older ones
            uint32_t k = 1;
            while (((n + k) < 17) && (z & (1u << (n + k)))) { ++k; }
            if ((n + k) >= 17) { k = 0xFF; }    // longer than the window

            if (4 == k)
            {
                data |= 1u << n;                // UF4 wrap gives a 1
            } else if (k > 4) {
                // no flux for >16us: random flux reversals (weak bits)
                const uint32_t p = gap_p[density & 3][(k > 11) ? 6 : (k - 5)];
                if ((xorshift32(rng) & 0xFF) < p) { data |= 1u << n; }
            }
        }
    }
    return (uint8_t) data;
}

/////////////////////////////////////////////////////////////////////

void gcr_write_byte(uint8_t* raw, uint32_t nbits, uint32_t q, uint8_t value)
{
    // first (MSB) bit goes to q-7
    uint32_t p = (q >= 7) ? (q - 7) : (q + nbits - 7);
    for (int n = 7; n >= 0; --n)
    {
        const uint8_t mask = (uint8_t) (0x80 >> (p & 7));
        if ((value >> n) & 1) { raw[p >> 3] |= mask; } else { raw[p >> 3] &= (uint8_t) ~mask; }
        if (++p >= nbits) { p = 0; }
    }
}


/////////////////////////////////////////////////////////////////////
// resampling at another density
//
// Event based version of the VICE read circuit model (rotation.c,
// rotation_1541_gcr(), GPL-2.0-or-later; see firmware/tests for the
// cycle based port it is checked against). Clock: 16MHz.
//  - every flux reversal resets UE7 to the density value and UF4 to 0
//    (one clock later, after the 2.5us filter)
//  - UE7 counts to 16 and reloads: one UF4 step every (16-density) clocks
//  - every 4th UF4 step shifts a bit: 1 right after a flux reversal, then
//    0 0 0, then 1 again (UF4 wraps) when there is no flux
//  - after ~18us without flux the analog part adds random flux reversals
//
// The output has a 1 only where the drive shifts in the bit that follows a
// real flux reversal. All other bits are 0 - also the UF4 wrap ones and the
// random ones in gaps without flux: the playback (gcr_fetch_byte) creates
// these again, random on every revolution like on a real drive. Protections
// check that such gaps read differently each time (RapidLok).

typedef struct {
    uint32_t ue7;       // UE7 counter (density .. 15)
    uint32_t uf4;       // UF4 counter (0..15)
    uint32_t fr;        // clocks until the next random flux reversal
    uint32_t dens;      // density = UE7 load value
    uint32_t rng;
    bool     flux;      // a real flux reversal came before the next bit
    uint8_t* out;
    uint32_t out_bits;  // bits written
    uint32_t max_bits;
} resample_t;

static inline uint32_t rs_rand(resample_t* r)
{
    return xorshift32(&r->rng) >> 16;
}

static inline void rs_overflow(resample_t* r)
{
    r->uf4 = (r->uf4 + 1) & 0xF;
    if (2 == (r->uf4 & 3))
    {
        if (r->out_bits < r->max_bits)
        {
            const uint8_t mask = (uint8_t) (0x80 >> (r->out_bits & 7));
            if (r->flux && (r->uf4 < 4)) { r->out[r->out_bits >> 3] |= mask; } else { r->out[r->out_bits >> 3] &= (uint8_t) ~mask; }
        }
        r->flux = false;
        ++r->out_bits;
    }
}

// k clocks of plain counting (no reset)
static void rs_count(resample_t* r, uint32_t k)
{
    const uint32_t period = 16 - r->dens;
    const uint32_t first  = 16 - r->ue7;    // clocks until the next overflow
    if (k < first)
    {
        r->ue7 += k;
        return;
    }
    k -= first;
    rs_overflow(r);
    while (k >= period)
    {
        k -= period;
        rs_overflow(r);
    }
    r->ue7 = r->dens + k;
}

// k clocks without a flux reversal (random reversals may occur)
static void rs_advance(resample_t* r, uint32_t k)
{
    while (k)
    {
        if (r->fr > k)
        {
            r->fr -= k;
            rs_count(r, k);
            return;
        }
        rs_count(r, r->fr - 1);
        k -= r->fr;
        // this clock: random flux reversal
        r->ue7 = r->dens;
        r->uf4 = 0;
        r->fr  = (rs_rand(r) % 367) + 33;
        rs_count(r, 1);
    }
}

uint32_t gcr_resample(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t out_max, uint8_t density, uint32_t* rot_bits)
{
    return gcr_resample_seeded(in, in_len, out, out_max, density, GCR_RESAMPLE_SEED, rot_bits);
}

uint32_t gcr_resample_seeded(const uint8_t* in, uint32_t in_len, uint8_t* out, uint32_t out_max, uint8_t density, uint32_t seed, uint32_t* rot_bits)
{
    const uint32_t nbits     = in_len << 3;
    const uint32_t count     = 3200000;     // 16MHz clocks per revolution (200ms)
    const uint32_t cyc_sum   = nbits;       // per clock: accumulator step (VICE: 8 * length)
    resample_t r;

    *rot_bits = 0;
    if (0 == in_len) { return 0; }

    // start right after a flux reversal: the decoder state is known there
    uint32_t start = 0;
    while ((start < nbits) && !raw_bit(in, (start + nbits - 1) % nbits)) { ++start; }
    if (start >= nbits) { start = 0; }

    r.ue7 = density; r.uf4 = 0; r.fr = 300; r.dens = density & 3; r.rng = seed ? seed : GCR_RESAMPLE_SEED; r.flux = false;
    r.out = out; r.out_bits = 0; r.max_bits = out_max << 3;

    // the head moves one bitcell when the accumulator passes count
    uint32_t accum   = 0;
    bool     pending = true;                // flux reversal before the start bit
    uint32_t q       = start;
    for (uint32_t n = 0; n < nbits; ++n)
    {
        if (0 == q) { *rot_bits = r.out_bits; }
        // clocks until the head reaches the end of this bitcell
        uint32_t need = (count - accum + cyc_sum - 1) / cyc_sum;
        accum = accum + need * cyc_sum - count;
        if (pending)
        {
            // filter delay done: UE7/UF4 reset, then the clock counts
            r.ue7  = r.dens;
            r.uf4  = 0;
            r.fr   = (rs_rand(&r) % 31) + 289;
            r.flux = true;
            rs_count(&r, 1);
            --need;
        }
        rs_advance(&r, need);
        pending = (0 != raw_bit(in, q));
        if (++q >= nbits) { q = 0; }
    }

    uint32_t out_len = r.out_bits >> 3;
    if (out_len > out_max) { out_len = out_max; }
    if (*rot_bits >= (out_len << 3)) { *rot_bits = 0; }
    return out_len;
}

void gcr_rotate_copy(uint8_t* dst, const uint8_t* src, uint32_t nbytes, uint32_t rot_bits)
{
    const uint32_t nbits = nbytes << 3;
    uint32_t q = rot_bits;
    for (uint32_t i = 0; i < nbytes; ++i)
    {
        uint32_t v = 0;
        for (int b = 0; b < 8; ++b)
        {
            v = (v << 1) | raw_bit(src, q);
            if (++q >= nbits) { q = 0; }
        }
        dst[i] = (uint8_t) v;
    }
}
