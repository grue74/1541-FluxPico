/**********************************
 * flux_math.h - timing math of the flux output (FLUX_MODE)
 *
 * Pure integer helpers with no Pico dependencies, so the host test in
 * firmware/tests can check them.
 *
 * The flux PIO program spends FLUX_CELL_CYCLES PIO cycles per bitcell.
 * A track of nbits bitcells is played in one revolution (200 ms, like
 * the bit-level playback before), so the PIO clock divider is
 *   div = clk_sys * (200 ms / nbits) / FLUX_CELL_CYCLES
 * kept as an 8.8 fixed point number (the PIO divider has 8 fraction bits).
 ***********************************/

#ifndef _FLUX_MATH_H_
#define _FLUX_MATH_H_

#include <stdint.h>
#include <stdbool.h>

#define FLUX_CELL_CYCLES    (64u)       // PIO cycles per bitcell
#define FLUX_REV_US         (200000u)   // one revolution at 300 rpm
#define FLUX_WIDTH_MAX      (31u)       // largest pulse delay field (W)

// PIO clock divider in 8.8 fixed point for a track of nbits bitcells:
// clk * 0.2 s / (64 * nbits) * 256 = clk * 4 / (5 * nbits)
static inline uint32_t flux_div_q8(uint32_t clk_hz, uint32_t nbits)
{
    if (0 == nbits)
    {
        return 256;
    }
    const uint64_t num = (uint64_t) clk_hz * 4u;
    const uint64_t den = (uint64_t) nbits * 5u;
    uint64_t d = (num + den / 2u) / den;
    if (d < 256u)
    {
        d = 256u;                       // divider >= 1.0
    }
    if (d > 0xFFFFFFu)
    {
        d = 0xFFFFFFu;                  // 16 integer bits
    }
    return (uint32_t) d;
}

// PIO clock divider for a bitcell of cell_ns nanoseconds (calibration pattern)
static inline uint32_t flux_div_q8_for_cell(uint32_t clk_hz, uint32_t cell_ns)
{
    // clk * cell / 64 * 256 = clk * cell * 4 / 1e9
    const uint64_t d = ((uint64_t) clk_hz * cell_ns * 4u + 500000000u) / 1000000000u;
    return (d < 256u) ? 256u : (uint32_t) d;
}

// bitcells the PIO plays in dt_us microseconds with divider div_q8:
// bits/s = clk * 256 / (div_q8 * 64) = clk * 4 / div_q8
static inline uint64_t flux_bits_in_us(uint32_t clk_hz, uint32_t div_q8, uint64_t dt_us)
{
    return (dt_us * (uint64_t) clk_hz * 4u) / ((uint64_t) div_q8 * 1000000u);
}

// pulse delay field W for a pulse of width_ns (the pulse lasts W+1 PIO cycles)
static inline uint32_t flux_width_field(uint32_t clk_hz, uint32_t div_q8, uint32_t width_ns)
{
    // PIO cycle = div / clk; cycles = width * clk / div
    const uint64_t cycles = ((uint64_t) width_ns * clk_hz * 256u + (uint64_t) div_q8 * 500000000u)
                            / ((uint64_t) div_q8 * 1000000000u);
    if (cycles <= 1u)
    {
        return 0u;
    }
    return (cycles - 1u > FLUX_WIDTH_MAX) ? FLUX_WIDTH_MAX : (uint32_t) (cycles - 1u);
}

// delays A and B of the two tail instructions: every path through the
// program takes W + A + B + 6 = FLUX_CELL_CYCLES cycles (delays are <= 31)
static inline void flux_tail_delays(uint32_t w, uint32_t* a, uint32_t* b)
{
    const uint32_t rest = FLUX_CELL_CYCLES - 6u - w;   // = A + B
    if (rest <= 31u)
    {
        *a = rest;
        *b = 0u;
    } else {
        *a = 31u;
        *b = rest - 31u;
    }
}

// byte offset in a track of len bytes where the disk angle at t_us is
// (the angle is global: a revolution every FLUX_REV_US, like before)
static inline uint32_t flux_angle_byte(uint64_t t_us, uint32_t len)
{
    if (0u == len)
    {
        return 0u;
    }
    const uint64_t in_rev = t_us % FLUX_REV_US;
    uint32_t off = (uint32_t) ((in_rev * len) / FLUX_REV_US);
    return (off >= len) ? (len - 1u) : off;
}

#endif
