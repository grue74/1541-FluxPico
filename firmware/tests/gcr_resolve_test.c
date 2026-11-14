/**********************************
 * host test for gcr_resolve.c
 *
 * build (MSVC):  cl /O2 /W3 /I..\include gcr_resolve_test.c ..\src\gcr_resolve.c
 * build (gcc):   gcc -O2 -Wall -I../include gcr_resolve_test.c ../src/gcr_resolve.c
 *
 * usage: gcr_resolve_test [file.g64 ...]
 *
 * tests:
 *  1. synthetic D64-style tracks (as convert_d64track2gcr builds them):
 *     all sectors must decode via the playback events, also when the
 *     track is rotated by 0..7 bits
 *  2. for every track of the given G64 files:
 *     a) playback events == straight per-bit simulation of the same model
 *     b) framing is invariant when the raw track is rotated by n bits
 *     c) comparison with a port of the VICE read circuit
 *        (rotation_1541_gcr, read branch): bytes between syncs must match
 ***********************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gcr_resolve.h"

#define MAXLEN  (0x2000)
#define MAXEV   (MAXLEN * 8)

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { ++failures; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static inline uint32_t bit_of(const uint8_t* raw, uint32_t q) { return (raw[q >> 3] >> (7 - (q & 7))) & 1; }

/////////////////////////////////////////////////////////////////////
// event list of one revolution, positions as raw bit positions

typedef struct { uint32_t pos; uint8_t type; uint8_t data; uint8_t weak; } ev_t;

static gcr_resolved_t res;
static uint8_t playback_density = 3;    // density for the gap model in gcr_fetch_byte

// play one revolution starting at raw position 0
static int playback(const uint8_t* raw, uint32_t len, const gcr_resolved_t* r, ev_t* ev, int* sync_at_start)
{
    gcr_cursor_t c;
    uint32_t rng = 1;
    const uint32_t nbits = len * 8;
    // offset of raw position 0, seek to the first event at raw pos >= 0
    uint32_t off0 = (nbits - r->origin) % nbits;
    c.rev = 0;
    // seek returns first event with offset > off; we want >= off0
    *sync_at_start = gcr_cursor_seek(r, &c, (off0 + nbits - 1) % nbits);
    int n = 0;
    uint64_t first = 0;
    for (;;)
    {
        uint32_t pos = gcr_raw_pos(r, c.off);
        // stop after one revolution
        uint64_t abs = (uint64_t) c.rev * nbits + r->origin + c.off;
        if (n == 0) { first = abs; }
        if (abs >= first + nbits) { break; }
        ev[n].pos  = pos;
        ev[n].type = c.type;
        ev[n].data = (c.type == GCR_EV_BYTE) ? gcr_fetch_byte(raw, nbits, pos, &rng, playback_density) : 0;
        ev[n].weak = 0;
        ++n;
        gcr_cursor_next(r, &c);
        if (n >= MAXEV) { break; }
    }
    return n;
}

// straight per-bit model: 3 revolutions from raw bit 0, record the last one
static int reference(const uint8_t* raw, uint32_t len, ev_t* ev)
{
    const uint32_t nbits = len * 8;
    uint32_t shift = 0, k = 0, bc = 0, n = 0;
    int in_sync = 0;
    uint32_t weakhist = 0;      // 1 = bit was weak
    uint32_t datahist = 0;
    for (uint32_t r = 0; r < 3; ++r)
    {
        for (uint32_t q = 0; q < nbits; ++q)
        {
            uint32_t d, w = 0;
            if (bit_of(raw, q)) { k = 0; d = 1; }
            else { if (k < 0xFFFF) ++k; d = ((k & 3) == 0); w = (k > 4); }
            shift = ((shift << 1) | d) & 0x3FF;
            datahist = (datahist << 1) | d;
            weakhist = (weakhist << 1) | w;
            if (shift == 0x3FF)
            {
                bc = 0;
                if (!in_sync) { in_sync = 1; if (r == 2) { ev[n].pos = q; ev[n].type = GCR_EV_SYNC_ON; ev[n].data = 0; ev[n].weak = 0; ++n; } }
            } else {
                if (in_sync) { in_sync = 0; if (r == 2) { ev[n].pos = q; ev[n].type = GCR_EV_SYNC_OFF; ev[n].data = 0; ev[n].weak = 0; ++n; } }
                if (++bc == 8)
                {
                    bc = 0;
                    if (r == 2) { ev[n].pos = q; ev[n].type = GCR_EV_BYTE; ev[n].data = (uint8_t) datahist; ev[n].weak = (weakhist & 0xFF) != 0; ++n; }
                }
            }
        }
    }
    return (int) n;
}

/////////////////////////////////////////////////////////////////////
// port of VICE rotation_1541_gcr() read branch (drive/rotation.c)
// VICE - the Versatile Commodore Emulator, GPL-2.0-or-later.
// rotation.c written by Andreas Boose; 1541 circuitry simulation code by
// Istvan Fabian and Benjamin 'BeRo' Rosseaux; GCR hardware tests by Peter Rittwage.

typedef struct { uint32_t pos; int sync; uint8_t data; } vev_t;

static uint32_t vice_xs;
static uint32_t vice_rand(void) { vice_xs ^= vice_xs << 13; vice_xs ^= vice_xs >> 17; return vice_xs ^= vice_xs << 5; }

// returns number of events in revolutions 1..2 (rev 0 is warm-up)
static int vice_run(const uint8_t* raw, uint32_t len, int dcba, vev_t* ev, int maxev)
{
    const uint32_t clk_ref_per_rev = 3200000;   // 16MHz / 5, rpm 300.00, no wobble
    const uint32_t count_new_bitcell = clk_ref_per_rev;
    const uint32_t cyc_sum_frv = 8 * len;
    uint32_t accum = 0, last_read_data = 0, fr_randcount = 0;
    int bit_counter = 0, ue7_counter = 0, uf4_counter = 0;
    int filter_counter = 0, filter_state = 0, filter_last_state = 0;
    uint32_t head = 0;
    int n = 0, was_sync = 0;
    vice_xs = 0x1234abcd;

    for (uint64_t cyc = 0; cyc < (uint64_t) clk_ref_per_rev * 3; ++cyc)
    {
        const int record = cyc >= clk_ref_per_rev;
        filter_counter += 1;
        if ((filter_counter >= 40) && (filter_last_state != filter_state))
        {
            filter_last_state = filter_state;
            ue7_counter = dcba;
            uf4_counter = 0;
            fr_randcount = ((vice_rand() >> 16) % 31) + 289;
        } else {
            fr_randcount -= 1;
            if (!fr_randcount)
            {
                ue7_counter = dcba;
                uf4_counter = 0;
                fr_randcount = ((vice_rand() >> 16) % 367) + 33;
            }
        }
        ue7_counter += 1;
        if (ue7_counter == 16)
        {
            ue7_counter = dcba;
            uf4_counter = (uf4_counter + 1) & 0xf;
            if ((uf4_counter & 0x3) == 2)
            {
                last_read_data = ((last_read_data << 1) & 0x3fe) | (((uf4_counter + 0x1c) >> 4) & 0x01);
                if (last_read_data == 0x3ff)
                {
                    bit_counter = 0;
                    if (!was_sync && record && n < maxev) { ev[n].pos = head; ev[n].sync = 1; ev[n].data = 0; ++n; }
                    was_sync = 1;
                } else {
                    was_sync = 0;
                    if (++bit_counter == 8)
                    {
                        bit_counter = 0;
                        if (record && n < maxev) { ev[n].pos = head; ev[n].sync = 0; ev[n].data = (uint8_t) last_read_data; ++n; }
                    }
                }
            }
        }
        accum += cyc_sum_frv;
        if (accum >= count_new_bitcell)
        {
            accum -= count_new_bitcell;
            uint32_t b = bit_of(raw, head);
            if (++head >= len * 8) { head = 0; }
            if (b)
            {
                filter_counter = 39;
                filter_state ^= 1;
            }
        }
    }
    return n;
}

/////////////////////////////////////////////////////////////////////

static uint8_t rot[MAXLEN];
static ev_t ev_a[MAXEV], ev_b[MAXEV];
static vev_t vev[MAXEV * 2];

static void rotate_bits(const uint8_t* src, uint8_t* dst, uint32_t len, uint32_t r)
{
    const uint32_t nbits = len * 8;
    memset(dst, 0, len);
    for (uint32_t q = 0; q < nbits; ++q)
    {
        if (bit_of(src, q))
        {
            uint32_t p = (q + r) % nbits;
            dst[p >> 3] |= (uint8_t) (0x80 >> (p & 7));
        }
    }
}

// compare playback against the per-bit reference
static void check_against_reference(const char* name, const uint8_t* raw, uint32_t len)
{
    int sync0;
    gcr_resolve(raw, len, &res);
    int na = playback(raw, len, &res, ev_a, &sync0);
    int nb = reference(raw, len, ev_b);

    // reference starts at raw 0 too, but may list events in a different
    // order only if positions are equal - compare as sorted-by-position lists
    int ia = 0, ib = 0, mism = 0;
    while (ia < na && ib < nb)
    {
        if (ev_a[ia].pos != ev_b[ib].pos || ev_a[ia].type != ev_b[ib].type ||
            (ev_a[ia].type == GCR_EV_BYTE && !ev_b[ib].weak && ev_a[ia].data != ev_b[ib].data))
        {
            if (mism < 3)
            {
                printf("  %s: event %d: play pos=%u type=%u data=%02X / ref pos=%u type=%u data=%02X weak=%u\n", name, ia,
                       ev_a[ia].pos, ev_a[ia].type, ev_a[ia].data, ev_b[ib].pos, ev_b[ib].type, ev_b[ib].data, ev_b[ib].weak);
            }
            ++mism;
        }
        ++ia; ++ib;
    }
    CHECK(mism == 0 && na == nb, "%s: playback vs reference: %d mismatches, %d vs %d events (segments %u%s)",
          name, mism, na, nb, res.nseg, res.overflow ? " OVERFLOW" : "");
}

// gcr_cursor_seek at random offsets must give the next event of the
// sequential playback and the correct SYNC state
static void check_seek(const char* name, const uint8_t* raw, uint32_t len)
{
    static uint32_t ev_off[MAXEV];
    static uint8_t  ev_type[MAXEV];
    gcr_resolve(raw, len, &res);
    const uint32_t nbits = len * 8;

    // all events of one revolution in offset order, from offset 0
    gcr_cursor_t c;
    c.rev = 0;
    gcr_cursor_seek(&res, &c, nbits - 1);   // first event of the next revolution
    const uint32_t rev0 = c.rev;
    int n = 0;
    while (c.rev == rev0 && n < MAXEV) { ev_off[n] = c.off; ev_type[n] = c.type; ++n; gcr_cursor_next(&res, &c); }

    uint32_t seed = len * 2654435761u;
    int bad = 0;
    for (int i = 0; i < 400; ++i)
    {
        seed = seed * 1664525u + 1013904223u;
        uint32_t off = seed % nbits;
        if (i < 2 * res.nseg && i < 200)
        {
            // targeted: inside a sync, and between sync_off and the first byte end
            const gcr_segment_t* s = &res.seg[i / 2];
            if (s->sync_off != GCR_RES_NO_OFFSET) { off = (i & 1) ? s->sync_off + 3 : s->sync_on + 2; }
            if (off >= nbits) { off = nbits - 1; }
        }
        gcr_cursor_t k;
        k.rev = 5;
        const bool in_sync = gcr_cursor_seek(&res, &k, off);

        int j = 0;
        while (j < n && ev_off[j] <= off) { ++j; }
        const uint32_t exp_off  = (j < n) ? ev_off[j] : ev_off[0];
        const uint8_t  exp_type = (j < n) ? ev_type[j] : ev_type[0];
        const uint32_t exp_rev  = (j < n) ? 5 : 6;

        bool exp_sync = false;
        for (uint32_t s = 0; s < res.nseg; ++s)
        {
            if (res.seg[s].sync_on <= off) { exp_sync = (res.seg[s].sync_off == GCR_RES_NO_OFFSET) || (off < res.seg[s].sync_off); }
        }
        if (k.off != exp_off || k.type != exp_type || k.rev != exp_rev || in_sync != exp_sync)
        {
            if (++bad <= 3)
            {
                printf("  %s: seek(%u): got off %u type %u rev %u sync %d, expected off %u type %u rev %u sync %d\n", name, off,
                       k.off, k.type, k.rev, in_sync, exp_off, exp_type, exp_rev, exp_sync);
            }
        }
    }
    CHECK(bad == 0, "%s: %d of 400 seeks wrong", name, bad);
}

// framing must move with the data when the track is rotated
static void check_rotation(const char* name, const uint8_t* raw, uint32_t len, uint32_t r)
{
    static gcr_resolved_t r1;
    gcr_resolve(raw, len, &res);
    rotate_bits(raw, rot, len, r);
    gcr_resolve(rot, len, &r1);
    int ok = (r1.nseg == res.nseg);
    if (ok && res.nseg > 0)
    {
        // same set of syncs, shifted by r: find seg in r1 matching seg 0 of res
        const uint32_t nbits = len * 8;
        uint32_t target = (gcr_raw_pos(&res, 0) + r) % nbits;
        uint32_t s1 = 0;
        while (s1 < r1.nseg && gcr_raw_pos(&r1, r1.seg[s1].sync_on) != target) { ++s1; }
        ok = (s1 < r1.nseg);
        for (uint32_t s = 0; ok && s < res.nseg; ++s)
        {
            const gcr_segment_t* a = &res.seg[s];
            const gcr_segment_t* b = &r1.seg[(s + s1) % r1.nseg];
            uint32_t pa_on  = (gcr_raw_pos(&res, a->sync_on) + r) % nbits;
            uint32_t pb_on  = gcr_raw_pos(&r1, b->sync_on);
            ok = (pa_on == pb_on);
            if (ok && a->sync_off != GCR_RES_NO_OFFSET)
            {
                ok = ((gcr_raw_pos(&res, a->sync_off) + r) % nbits) == gcr_raw_pos(&r1, b->sync_off);
            }
        }
    }
    CHECK(ok, "%s: framing changes when rotating by %u bits", name, r);
}

/////////////////////////////////////////////////////////////////////
// compare with the VICE model: for every sync, the bytes that follow

static int vice_compare(const char* name, const uint8_t* raw, uint32_t len, int zone, int* compared, int* weak_diff)
{
    int sync0;
    gcr_resolve(raw, len, &res);
    int na = playback(raw, len, &res, ev_a, &sync0);
    (void) reference(raw, len, ev_b);   // gives weak flags (same positions as playback)
    playback_density = (uint8_t) (3 - zone);
    int nv = vice_run(raw, len, 3 - zone, vev, MAXEV * 2);
    const uint32_t nbits = len * 8;
    int bad_segments = 0;
    *compared = 0;

    // for each SYNC_OFF in our events, find VICE sync near the same position
    for (int i = 0; i < na; ++i)
    {
        if (ev_a[i].type != GCR_EV_SYNC_ON) { continue; }
        // our bytes until next sync
        uint8_t ours[64]; uint8_t weak[64]; int no = 0;
        // (circular: the last segment continues at the start of the list)
        for (int j = (i + 1) % na; ev_a[j].type != GCR_EV_SYNC_ON && no < 64; j = (j + 1) % na)
        {
            if (ev_a[j].type == GCR_EV_BYTE) { weak[no] = ev_b[j].weak; ours[no++] = ev_a[j].data; }
        }
        if (no == 0) { continue; }
        // VICE: nearest sync-on event within +-24 bits (head runs ahead of shifter),
        // earliest best match, so it is in the first recorded revolution
        // and the following bytes exist
        int v = -1;
        int32_t best = 25;
        for (int j = 0; j < nv; ++j)
        {
            if (!vev[j].sync) { continue; }
            int32_t d = (int32_t) vev[j].pos - (int32_t) ev_a[i].pos;
            if (d < -(int32_t) nbits / 2) d += nbits;
            if (d > (int32_t) nbits / 2) d -= nbits;
            if (d < 0) d = -d;
            if (d < best) { best = d; v = j; }
        }
        if (v < 0) { ++bad_segments; if (bad_segments <= 3) printf("  %s: no VICE sync near %u\n", name, ev_a[i].pos); continue; }
        int mism = 0, k = 0;
        for (int j = v + 1; j < nv && !vev[j].sync && k < no; ++j, ++k)
        {
            if (!weak[k] && vev[j].data != ours[k]) { ++mism; }
        }
        // weak bits are random in VICE (random flux reversals) and can also
        // shift the framing behind them: count those segments separately
        int anyweak = 0;
        for (int x = 0; x < no; ++x) { anyweak |= weak[x]; }
        if (mism && anyweak) { ++*weak_diff; }
        if (mism && !anyweak)
        {
            ++bad_segments;
            if (bad_segments <= 3)
            {
                printf("  %s: segment at %u: %d byte mismatches vs VICE\n    ours:", name, ev_a[i].pos, mism);
                for (int x = 0; x < no && x < 24; ++x) printf(" %02X%s", ours[x], weak[x] ? "w" : "");
                printf("\n    vice:");
                k = 0;
                for (int j = v + 1; j < nv && !vev[j].sync && k < 24; ++j, ++k) printf(" %02X", vev[j].data);
                printf("\n");
            }
        }
        ++*compared;
    }
    return bad_segments;
}

/////////////////////////////////////////////////////////////////////
// synthetic D64 track as built by convert_d64track2gcr()

static const uint8_t gcr_tab[16] = { 0x0a, 0x0b, 0x12, 0x13, 0x0e, 0x0f, 0x16, 0x17, 0x09, 0x19, 0x1a, 0x1b, 0x0d, 0x1d, 0x1e, 0x15 };

static void to_gcr(const uint8_t* in4, uint8_t* out5)
{
    uint64_t v = 0;
    for (int i = 0; i < 4; ++i) { v = (v << 10) | ((uint64_t) gcr_tab[in4[i] >> 4] << 5) | gcr_tab[in4[i] & 15]; }
    for (int i = 4; i >= 0; --i) { out5[i] = (uint8_t) v; v >>= 8; }
}

static int from_gcr(const uint8_t* in5, uint8_t* out4)
{
    uint64_t v = 0;
    for (int i = 0; i < 5; ++i) { v = (v << 8) | in5[i]; }
    for (int i = 3; i >= 0; --i)
    {
        int lo = -1, hi = -1;
        for (int n = 0; n < 16; ++n) { if (gcr_tab[n] == (v & 31)) lo = n; if (gcr_tab[n] == ((v >> 5) & 31)) hi = n; }
        if (lo < 0 || hi < 0) { return 0; }
        out4[i] = (uint8_t) ((hi << 4) | lo);
        v >>= 10;
    }
    return 1;
}

static uint32_t build_d64_track(uint8_t* t, int track, int nsec, int gap, uint8_t id1, uint8_t id2)
{
    uint8_t* p = t;
    for (int s = 0; s < nsec; ++s)
    {
        uint8_t b[4], sec[260];
        memset(p, 0xFF, 5); p += 5;
        b[0] = 0x08; b[1] = (uint8_t) (s ^ track ^ id1 ^ id2); b[2] = (uint8_t) s; b[3] = (uint8_t) track; to_gcr(b, p); p += 5;
        b[0] = id2; b[1] = id1; b[2] = 0x0F; b[3] = 0x0F; to_gcr(b, p); p += 5;
        memset(p, 0x55, 9); p += 9;
        memset(p, 0xFF, 5); p += 5;
        sec[0] = 0x07;
        uint8_t sum = 0;
        for (int i = 1; i <= 256; ++i) { sec[i] = (uint8_t) (track * 37 + s * 11 + i); sum ^= sec[i]; }
        sec[257] = sum; sec[258] = 0; sec[259] = 0;
        for (int i = 0; i < 260; i += 4) { to_gcr(&sec[i], p); p += 5; }
        memset(p, 0x55, gap); p += gap;
    }
    return (uint32_t) (p - t);
}

// decode all sectors from the event stream like the DOS would
static int decode_sectors(const ev_t* ev, int n, int track, int nsec, uint32_t* found_mask)
{
    int good = 0;
    *found_mask = 0;
    for (int i = 0; i < n; ++i)
    {
        if (ev[i].type != GCR_EV_SYNC_OFF) { continue; }
        uint8_t g[330]; int k = 0;
        for (int j = i + 1; j < n && ev[j].type == GCR_EV_BYTE && k < 330; ++j) { g[k++] = ev[j].data; }
        uint8_t d[264];
        if (k >= 10 && from_gcr(g, d) && d[0] == 0x08)
        {
            if (d[3] == track && d[2] < nsec && d[1] == (d[2] ^ d[3] ^ d[4] ^ d[5])) { /* header ok */ }
        } else if (k >= 325 && from_gcr(g, d) && d[0] == 0x07) {
            int okc = 1;
            for (int b = 0; b < 65 && okc; ++b) okc = from_gcr(&g[b * 5], &d[b * 4]);
            uint8_t sum = 0;
            for (int b = 1; b <= 256; ++b) sum ^= d[b];
            if (okc && sum == d[257])
            {
                int s = -1;
                for (int x = 0; x < nsec; ++x) if (d[1] == (uint8_t) (track * 37 + x * 11 + 1)) s = x;
                if (s >= 0) { *found_mask |= 1u << s; ++good; }
            }
        }
    }
    return good;
}

static void test_d64_tracks(void)
{
    static uint8_t t[MAXLEN];
    static const int nsec[4] = { 21, 19, 18, 17 };
    static const int gaps[4] = { 12, 21, 16, 13 };
    static const int old_us[4] = { 26, 28, 30, 32 };
    printf("synthetic D64 tracks:\n");
    for (int zone = 0; zone < 4; ++zone)
    {
        uint32_t len = build_d64_track(t, zone * 8 + 1, nsec[zone], gaps[zone], 'A', 'B');
        for (uint32_t r = 0; r < 8; ++r)
        {
            int sync0;
            rotate_bits(t, rot, len, r * 997 + r);
            gcr_resolve(rot, len, &res);
            int n = playback(rot, len, &res, ev_a, &sync0);
            // two revolutions, so the sector crossing the track end is complete
            memcpy(&ev_a[n], ev_a, n * sizeof(ev_t));
            n *= 2;
            uint32_t mask;
            int good = decode_sectors(ev_a, n, zone * 8 + 1, nsec[zone], &mask);
            CHECK(mask == (1u << nsec[zone]) - 1, "zone %d rot %u: sector mask %06X (%d decoded)", zone, r, mask, good);
        }
        printf("  zone %d: %u bytes, %u syncs, byte time %.2fus (old fixed %dus)\n", zone, len, res.nseg,
               8.0 * GCR_REVOLUTION_US / (len * 8.0), old_us[zone]);
    }
}

/////////////////////////////////////////////////////////////////////

static uint8_t img[1 << 20];

static void test_g64(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); ++failures; return; }
    size_t sz = fread(img, 1, sizeof(img), f);
    fclose(f);
    const char* base = strrchr(path, '\\'); if (!base) base = strrchr(path, '/'); base = base ? base + 1 : path;
    if (sz < 12 || memcmp(img, "GCR-1541", 8)) { printf("%s: not a G64\n", base); return; }

    const uint32_t nht = img[9];
    int tracks = 0, segs = 0, vice_bad = 0, vice_cmp = 0, vice_weak = 0, overflow = 0, nosync = 0;
    double t_resolve = 0, t_max = 0;
    for (uint32_t ht = 0; ht < nht; ++ht)
    {
        uint32_t off; memcpy(&off, &img[12 + ht * 4], 4);
        if (!off || off + 2 > sz) { continue; }
        uint32_t len = img[off] | (img[off + 1] << 8);
        if (!len || len > MAXLEN || off + 2 + len > sz) { continue; }
        uint32_t speed; memcpy(&speed, &img[12 + nht * 4 + ht * 4], 4);
        const uint8_t* raw = &img[off + 2];
        char name[64]; snprintf(name, sizeof(name), "track %.1f", ht / 2.0 + 1);
        // decoder clock as the DOS sets it by track number (the speed table
        // of the image is not seen by a real drive and is often bogus)
        (void) speed;
        int zone = (ht / 2 < 17 ? 0 : ht / 2 < 24 ? 1 : ht / 2 < 30 ? 2 : 3);
        // tracks recorded at another density are read by protection loaders
        // with matching density bits: use the zone nearest to the track length
        {
            static const int nom[4] = { 7692, 7142, 6666, 6250 };
            int best = zone;
            for (int z = 0; z < 4; ++z) { if (abs((int) len - nom[z]) < abs((int) len - nom[best])) best = z; }
            if (abs((int) len - nom[zone]) > 300) { zone = best; }
        }

        clock_t c0 = clock();
        const int reps = 5;
        for (int r = 0; r < reps; ++r) { gcr_resolve(raw, len, &res); }
        double dt = (double) (clock() - c0) / CLOCKS_PER_SEC / reps * 1000.0;
        t_resolve += dt; if (dt > t_max) t_max = dt;

        ++tracks; segs += res.nseg; overflow += res.overflow; nosync += (res.nseg == 0);
        check_against_reference(name, raw, len);
        check_seek(name, raw, len);
        check_rotation(name, raw, len, 1 + ht % 7);
        check_rotation(name, raw, len, 8 * (ht + 3) + 5);
        int cmp;
        int wd = 0;
        vice_bad += vice_compare(name, raw, len, zone, &cmp, &wd);
        vice_weak += wd;
        vice_cmp += cmp;
    }
    printf("%s: %d tracks, %d syncs, %d no-sync, %d overflow, resolve avg %.2fms max %.2fms (host), VICE: %d/%d segments differ (+%d with weak bits)\n",
           base, tracks, segs, nosync, overflow, t_resolve / (tracks ? tracks : 1), t_max, vice_bad, vice_cmp, vice_weak);
}

/////////////////////////////////////////////////////////////////////
// reading at another density (gcr_resample)
//
// The VICE model reads the recorded track with the decoder clock of the
// selected density. gcr_resample() turns the track into the bit stream the
// decoder produces; the firmware plays that stream at its own length.
// Both must give the same bytes behind every sync (except weak bits, which
// are random in VICE and fixed in the resampled stream).

static int verbose_density = 0;
static uint8_t rs_out[8192];
static uint8_t rs_rot[8192];
static vev_t   vev2[MAXEV * 2];

// load a track of a G64 image: returns its length, 0 if missing
static uint32_t g64_track(const uint8_t* g, uint32_t half, const uint8_t** data)
{
    const uint32_t nht = g[9];
    if (half >= nht) { return 0; }
    uint32_t off; memcpy(&off, &g[12 + half * 4], 4);
    if (!off) { return 0; }
    *data = &g[off + 2];
    return (uint32_t) (g[off] | (g[off + 1] << 8));
}

// resample one track; returns the new length (rotated to keep the angle)
static uint32_t resample_track(const uint8_t* raw, uint32_t len, int dens, uint8_t* dst)
{
    uint32_t rot;
    const uint32_t nlen = gcr_resample(raw, len, rs_out, sizeof(rs_out), (uint8_t) dens, &rot);
    gcr_rotate_copy(dst, rs_out, nlen, rot);
    return nlen;
}

static void density_check(const char* path, int first, int last, int dens)
{
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); ++failures; return; }
    const size_t sz = fread(img, 1, sizeof(img), f);
    fclose(f);
    (void) sz;
    printf("density check %s, tracks %d-%d read at density %d\n", path, first, last, dens);

    for (int t = first; t <= last; ++t)
    {
        const uint8_t* raw;
        const uint32_t len = g64_track(img, (uint32_t) (t - 1) * 2, &raw);
        if (!len) { continue; }
        const uint32_t nlen = resample_track(raw, len, dens, rs_rot);
        playback_density = (uint8_t) dens;

        // VICE: decoder at density dens on the recorded track
        const int nv = vice_run(raw, len, dens, vev2, MAXEV * 2);
        // ours: resampled stream through the firmware resolver/playback
        int s0;
        gcr_resolve(rs_rot, nlen, &res);
        const int na = playback(rs_rot, nlen, &res, ev_a, &s0);

        // segments in time order: VICE (first recorded revolution) and ours
        int segs = 0, same = 0, weakish = 0, first_bad = -1;
        uint32_t vbytes_max = 0, obytes_max = 0;
        for (int i = 0; i < na; ++i)
        {
            if (GCR_EV_SYNC_ON != ev_a[i].type) { continue; }
            const double tf = (double) ev_a[i].pos / (nlen * 8.0);
            // our bytes
            uint8_t ours[400]; int no = 0;
            for (int j = (i + 1) % na; ev_a[j].type != GCR_EV_SYNC_ON && no < 400; j = (j + 1) % na)
            {
                if (GCR_EV_BYTE == ev_a[j].type) { ours[no++] = ev_a[j].data; }
            }
            // nearest VICE sync by time
            int v = -1; double best = 0.004;
            for (int j = 0; j < nv; ++j)
            {
                if (!vev2[j].sync) { continue; }
                double d = (double) vev2[j].pos / (len * 8.0) - tf;
                if (d < -0.5) d += 1.0;
                if (d > 0.5) d -= 1.0;
                if (d < 0) d = -d;
                if (d < best) { best = d; v = j; }
            }
            ++segs;
            if (v < 0) { if (first_bad < 0) first_bad = i; continue; }
            int k = 0, mism = 0;
            uint8_t vb[400];
            for (int j = v + 1; j < nv && !vev2[j].sync && k < 400; ++j) { vb[k++] = vev2[j].data; }
            const int nc = (k < no) ? k : no;
            int firstdiff = -1;
            for (int x = 0; x < nc && x < 330; ++x) { if ((vb[x] != ours[x]) && (firstdiff < 0)) firstdiff = x; mism += (vb[x] != ours[x]); }
            if (verbose_density) printf("    track %d seg %.4f: %3d/%3d bytes, first diff at %d, first byte %02X\n", t, tf, no, k, firstdiff, no ? ours[0] : 0);
            if ((uint32_t) k > vbytes_max) vbytes_max = (uint32_t) k;
            if ((uint32_t) no > obytes_max) obytes_max = (uint32_t) no;
            // headers end in gaps without flux: random in VICE, fixed in the
            // resampled stream - only differences after byte 8 are allowed
            if (0 == mism) { ++same; }
            else if (firstdiff >= 8) { ++weakish; }
            else if (first_bad < 0)
            {
                first_bad = i;
                printf("    track %d segment at %.4f: %d differences\n      ours:", t, tf, mism);
                for (int x = 0; x < 20 && x < no; ++x) printf(" %02X", ours[x]);
                printf("\n      vice:");
                for (int x = 0; x < 20 && x < k; ++x) printf(" %02X", vb[x]);
                printf("\n");
            }
        }
        printf("  track %2d: %u -> %u bytes, %d syncs, %d same as VICE, %d with a few (weak) differences, longest segment ours %u / VICE %u bytes\n",
               t, len, nlen, segs, same, weakish, obytes_max, vbytes_max);
        CHECK((same + weakish) >= segs - 1, "track %d: segments differ from VICE", t);
    }
}

// VICE reading the converted track at density dens: does it decode the
// same bytes as VICE reading the original track? (a converted image in
// VICE is decoded twice - this shows whether that changes the bytes)
static vev_t vev3[MAXEV * 2];
static void density_vice(const char* path, int first, int last, int dens)
{
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); ++failures; return; }
    (void) fread(img, 1, sizeof(img), f);
    fclose(f);
    for (int t = first; t <= last; ++t)
    {
        const uint8_t* raw;
        const uint32_t len = g64_track(img, (uint32_t) (t - 1) * 2, &raw);
        if (!len) { continue; }
        const uint32_t nlen = resample_track(raw, len, dens, rs_rot);
        const int na = vice_run(raw, len, dens, vev2, MAXEV * 2);
        const int nb = vice_run(rs_rot, nlen, dens, vev3, MAXEV * 2);
        // compare the bytes behind each sync, syncs matched by angle
        int segs = 0, same = 0, late = 0;
        for (int i = 0; i < na; ++i)
        {
            if (!vev2[i].sync) { continue; }
            const double ta = (double) vev2[i].pos / (len * 8.0);
            int j = -1; double best = 0.004;
            for (int k = 0; k < nb; ++k)
            {
                if (!vev3[k].sync) { continue; }
                double dd = (double) vev3[k].pos / (nlen * 8.0) - ta;
                if (dd < -0.5) dd += 1.0;
                if (dd > 0.5) dd -= 1.0;
                if (dd < 0) dd = -dd;
                if (dd < best) { best = dd; j = k; }
            }
            ++segs;
            if (j < 0) { continue; }
            int x = 0, firstdiff = -1;
            for (int a = i + 1, b = j + 1; a < na && b < nb && !vev2[a].sync && !vev3[b].sync && x < 330; ++a, ++b, ++x)
            {
                if ((vev2[a].data != vev3[b].data) && (firstdiff < 0)) { firstdiff = x; }
            }
            if (firstdiff < 0) { ++same; } else if (firstdiff >= 8) { ++late; }
            else if (segs <= 400) { printf("    track %d sync at %.4f: first difference at byte %d\n", t, ta, firstdiff); }
        }
        printf("  track %2d: VICE on original vs VICE on converted: %d syncs, %d same, %d differ only after byte 8\n",
               t, segs, same, late);
    }
}

// write a G64 with tracks first..last resampled at density dens (for VICE)
static void density_write(const char* in, const char* out, int first, int last, int dens)
{
    FILE* f = fopen(in, "rb");
    if (!f) { printf("cannot open %s\n", in); ++failures; return; }
    (void) fread(img, 1, sizeof(img), f);
    fclose(f);
    const uint32_t nht = img[9];
    const uint32_t maxlen = 7928;
    static uint8_t o[1 << 20];
    memcpy(o, img, 12);
    o[10] = (uint8_t) (maxlen & 0xFF); o[11] = (uint8_t) (maxlen >> 8);
    uint32_t pos = 12 + nht * 8;
    for (uint32_t ht = 0; ht < nht; ++ht)
    {
        const uint8_t* raw;
        uint32_t len = g64_track(img, ht, &raw);
        uint32_t speed; memcpy(&speed, &img[12 + nht * 4 + ht * 4], 4);
        uint32_t off = 0;
        if (len)
        {
            const int t = (int) (ht / 2) + 1;
            static uint8_t tbuf[8192];
            if ((t >= first) && (t <= last))
            {
                len = resample_track(raw, len, dens, tbuf);
                speed = (uint32_t) dens;
            } else {
                if (len > maxlen) len = maxlen;
                memcpy(tbuf, raw, len);
            }
            off = pos;
            o[pos] = (uint8_t) (len & 0xFF); o[pos + 1] = (uint8_t) (len >> 8);
            memset(&o[pos + 2], 0x55, maxlen);
            memcpy(&o[pos + 2], tbuf, len);
            pos += 2 + maxlen;
        }
        memcpy(&o[12 + ht * 4], &off, 4);
        memcpy(&o[12 + nht * 4 + ht * 4], &speed, 4);
    }
    f = fopen(out, "wb");
    fwrite(o, 1, pos, f);
    fclose(f);
    printf("wrote %s (%u bytes)\n", out, pos);
}

/////////////////////////////////////////////////////////////////////

int main(int argc, char** argv)
{
    // gcr_resolve_test --density-check image.g64 first last density
    // gcr_resolve_test --density-write in.g64 out.g64 first last density
    if ((argc == 6) && (0 == strcmp(argv[1], "--density-check")))
    {
        density_check(argv[2], atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
        printf(failures ? "\n%d FAILURES\n" : "\nALL OK\n", failures);
        return failures ? 1 : 0;
    }
    // gcr_resolve_test --dump image.g64 track density: bytes after the first
    // sync, VICE on the original vs firmware playback of the converted track
    if ((argc == 5) && (0 == strcmp(argv[1], "--dump")))
    {
        FILE* f = fopen(argv[2], "rb");
        (void) fread(img, 1, sizeof(img), f);
        fclose(f);
        const int t = atoi(argv[3]), dens = atoi(argv[4]);
        const uint8_t* raw;
        const uint32_t len = g64_track(img, (uint32_t) (t - 1) * 2, &raw);
        const uint32_t nlen = resample_track(raw, len, dens, rs_rot);
        for (int seed = 1; seed <= 4; ++seed)
        {
            vice_xs = 0;   // reset below
            int nv;
            {
                // vice_run seeds its own generator: vary it through the start rotation
                static uint8_t rotd[MAXLEN];
                rotate_bits(raw, rotd, len, (uint32_t) seed * 977u);
                nv = vice_run(rotd, len, dens, vev2, MAXEV * 2);
            }
            for (int i = 0; i < nv; ++i)
            {
                if (!vev2[i].sync) { continue; }
                printf("VICE  %d:", seed);
                for (int j = i + 1, k = 0; j < nv && !vev2[j].sync && k < 64; ++j, ++k) printf(" %02X", vev2[j].data);
                printf("\n");
                break;
            }
        }
        gcr_resolve(rs_rot, nlen, &res);
        playback_density = (uint8_t) dens;
        for (int seed = 1; seed <= 4; ++seed)
        {
            gcr_cursor_t c; c.rev = 0;
            uint32_t rng = 0x9E3779B9u * (uint32_t) seed;
            gcr_cursor_seek(&res, &c, 0);
            while (c.type != GCR_EV_SYNC_OFF) gcr_cursor_next(&res, &c);
            gcr_cursor_next(&res, &c);
            printf("ours  %d:", seed);
            for (int k = 0; k < 64; ++k)
            {
                printf(" %02X", gcr_fetch_byte(rs_rot, nlen * 8, gcr_raw_pos(&res, c.off), &rng, (uint8_t) dens));
                gcr_cursor_next(&res, &c);
            }
            printf("\n");
        }
        return 0;
    }
    if ((argc == 6) && (0 == strcmp(argv[1], "--density-vice")))
    {
        density_vice(argv[2], atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));
        return 0;
    }
    if ((argc == 7) && (0 == strcmp(argv[1], "--density-write")))
    {
        density_write(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]), atoi(argv[6]));
        return 0;
    }

    test_d64_tracks();

    // special tracks
    {
        static uint8_t t[7692];
        memset(t, 0xFF, sizeof(t));
        gcr_resolve(t, sizeof(t), &res);
        CHECK(res.nseg == 1 && res.seg[0].sync_off == GCR_RES_NO_OFFSET, "all-sync track");
        memset(t, 0x00, sizeof(t));
        gcr_resolve(t, sizeof(t), &res);
        CHECK(res.nseg == 0, "all-zero track");
        check_against_reference("all-zero", t, sizeof(t));
        memset(t, 0x55, sizeof(t));
        gcr_resolve(t, sizeof(t), &res);
        CHECK(res.nseg == 0, "0x55 track");
        check_against_reference("0x55", t, sizeof(t));
        // sync crossing the end of the track, unaligned
        memset(t, 0x55, sizeof(t));
        t[sizeof(t) - 1] = 0x0F; t[0] = 0xFF; t[1] = 0xC9;
        check_against_reference("wrap-sync", t, sizeof(t));
        check_rotation("wrap-sync", t, sizeof(t), 3);
    }

    for (int i = 1; i < argc; ++i) { test_g64(argv[i]); }

    printf(failures ? "\n%d FAILURES\n" : "\nALL OK\n", failures);
    return failures ? 1 : 0;
}
