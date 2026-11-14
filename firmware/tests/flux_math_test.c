// host test for include/flux_math.h (FLUX_MODE timing)
// build: cl /I..\include flux_math_test.c   or   cc -I../include flux_math_test.c

#include <stdio.h>
#include <stdlib.h>
#include "flux_math.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    const uint32_t clk = 150000000u;
    // nominal track lengths of the four zones (bytes) and their bitcells
    const uint32_t lens[4]    = { 7692, 7142, 6666, 6250 };
    const uint32_t cell_ns[4] = { 3250, 3500, 3750, 4000 };

    for (int z = 0; z < 4; ++z)
    {
        const uint32_t nbits = lens[z] * 8u;
        const uint32_t div = flux_div_q8(clk, nbits);
        // one revolution (200 ms) plays the whole track: error < 0.2 %
        const uint64_t bits = flux_bits_in_us(clk, div, FLUX_REV_US);
        const double err = ((double) bits - nbits) / nbits;
        CHECK(err < 0.002 && err > -0.002, "zone len %u: %llu bits in 200 ms, want %u", lens[z], (unsigned long long) bits, nbits);
        // bitcell = div * 64 / clk
        const double cell = (double) div / 256.0 * FLUX_CELL_CYCLES / clk * 1e9;
        CHECK(cell > cell_ns[z] - 5 && cell < cell_ns[z] + 5, "len %u: cell %.1f ns, want %u", lens[z], cell, cell_ns[z]);
        // the calibration divider for the same cell matches
        const uint32_t dc = flux_div_q8_for_cell(clk, cell_ns[z]);
        CHECK(abs((int) dc - (int) div) <= 2, "cell %u: div %u vs %u", cell_ns[z], dc, div);
        printf("zone len %u: div %u.%03u, cell %.1f ns, %llu bits/rev\n", lens[z], div >> 8, (div & 255) * 1000 / 256, cell, (unsigned long long) bits);

        // pulse widths: W+1 PIO cycles close to the wanted width, all paths 64 cycles
        const uint32_t widths[] = { 100, 200, 400, 700, 1000, 1500, 3000 };
        for (unsigned i = 0; i < sizeof(widths) / sizeof(widths[0]); ++i)
        {
            const uint32_t w = flux_width_field(clk, div, widths[i]);
            uint32_t a, b;
            flux_tail_delays(w, &a, &b);
            CHECK(w <= 31 && a <= 31 && b <= 31, "delays out of range w %u a %u b %u", w, a, b);
            // paths of the PIO program (see flux.c): 1 read, 1 write, 0
            const uint32_t pulse_path = 1 + 1 + 1 + (w + 1) + (a + 1) + (b + 1);
            const uint32_t write_path = 1 + 1 + 1 + (w + 1) + (a + 1) + (b + 1);
            const uint32_t zero_path  = 1 + 1 + (w + 1) + 1 + (a + 1) + (b + 1);
            CHECK(pulse_path == FLUX_CELL_CYCLES && write_path == FLUX_CELL_CYCLES && zero_path == FLUX_CELL_CYCLES,
                  "paths %u %u %u", pulse_path, write_path, zero_path);
            const double pio_ns = (double) div / 256.0 / clk * 1e9;
            const double got = (w + 1) * pio_ns;
            if (widths[i] < 1500)
            {
                CHECK(got > widths[i] - pio_ns && got < widths[i] + pio_ns, "width %u: got %.0f ns", widths[i], got);
            }
        }
    }

    // angle: the same time gives the same fraction of the track
    CHECK(flux_angle_byte(0, 7692) == 0, "angle 0");
    CHECK(flux_angle_byte(100000, 7692) == 3846, "angle half");
    CHECK(flux_angle_byte(300000, 6250) == 3125, "angle half, next revolution");
    CHECK(flux_angle_byte(199999, 100) == 99, "angle end");
    CHECK(flux_angle_byte(5, 0) == 0, "empty track");

    printf(fails ? "%d FAILED\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
