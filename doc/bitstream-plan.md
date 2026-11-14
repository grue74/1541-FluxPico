# Plan: flux stream into the drive's own read circuit (1541-FluxPico)

Goal: the Pico no longer plays bytes. It holds each track as a **flux stream** (recorded at the
track's own rate) and sends it as pulses into the **drive's own read circuit**, in place of the
read amplifier's output. The drive's gate array then does everything as with a real disk: it decodes
at the density the DOS/loader selects (DS0/DS1 go to it directly), detects SYNC, shifts the bytes
onto VIA port A and pulses BYTE READY.

What this removes: `.den` files, the weak-bit table, the byte resolver, the timer-driven byte
playback and its interrupt-latency problems, and the need to see DS0/DS1 at all.

What it costs: the board is still plugged into the VIA socket, but it needs a few wires and one cut
on the drive's main board, and these differ per drive model. The first target is the **1541-II
(ASSY 340503)**. 1541-I long/short boards (discrete read circuit) come later.

---

## How the 1541-II reads today (schematic 340503)

* **U12** (CX20185 R/W hybrid) amplifies the head signal. Its pin 33 *Read Data* goes straight to the
  gate array **U10 pin 33 (CLR)**: every flux reversal restarts the bit-cell counters.
* **U10** (251828 gate array) decodes with the 16 MHz clock and the density from DS0/DS1 (pins
  15/16), gives SYNC (pin 17 -> VIA PB7), the byte (YB0-7 -> VIA PA) and BYTE READY (pin 42 -> VIA CA1
  and the 6502 SO pin). Its **pin 19 (OE)** is the VIA's CB2 (MODE): high = read (drives PA),
  low = write. **Pin 41** is SOE (VIA CA2).
* Writing: the gate array's **pin 40 (B)** changes level for every flux reversal to be written; U11
  (LS86) and U14 (LS14) turn the edges into pulses for U12 *Write Data* (pin 29).

## How the rePico board (v1.7) keeps the gate array out today

* **P1 pin 19 is wired to GND** (the VIA's MODE stays on the board). The gate array always sees
  write mode, so it never drives PA and never pulses BYTE READY. The Pico drives PA (through the
  74LVC4245), BYTE READY and SYNC itself.
* **SOE** (VIA CA2) goes only to the Pico. The gate array gets SOE_GA from the Pico (GPIO3 via Q9),
  always low.
* PA0-7, SYNC (PB7), BRDY (CA1), DS0/DS1, stepper and motor lines pass straight through the socket.

## Phase 0 - pulse shape, found by the Pico itself

There is no working drive mechanism to measure U12's *Read Data* pulses. U12 makes them with its own
one-shot (pins 36/37 "MMVA", 39k/2k4), so they are pulses of a fixed width per flux reversal;
polarity and width are unknown. The Pico can find them without instruments, because in flux mode it
still sees the gate array's outputs: SYNC (GPIO2), BYTE READY (GPIO22) and port A (GPIO8-15, the
74LVC4245 set to inputs).

* calibration mode: motor on, DOS idle (read mode), play a known track (a sync followed by known
  GCR bytes)
* sweep polarity (active low/high) and pulse width (0.1-2 us); for each setting check that SYNC
  comes at the right places and that port A shows the expected bytes
* keep the widest working range and use its middle; show the result on the display and in the log

## Phase 1 - modification (PCB v1.7 + 1541-II)

Step-by-step with pictures: `doc/flux-mod-1541-II.pdf` (generator `tools/mod_pdf/make_flux_pdf.py`).
The 1541-II has the **40-pin 251828-03** gate array: CLR = pin 31, OE (MODE) = pin 19, SOE = pin 39,
BYTE = pin 40 (the schematic shows the 42-pin numbering, CLR = 33).

| # | What | Why |
|---|---|---|
| 1 | Gate array pin 31 (CLR): cut the leg near the board, wire C to the leg | U12's Read Data would fight the Pico |
| 2 | rePico: clip plug pins P1.19 and P2.19 flush | P1.19 tied MODE to GND; P2.19 carried the Pico's SOE_GA |
| 3 | rePico: lift Q1 leg 3 (drain) | Q1 turned the 74LVC4245 towards the VIA in read mode; with it off, R4 (1M to 5V) keeps DIR high = VIA -> Pico, so the gate array can drive port A |
| 4 | wire A: VIA socket pin 19 (MODE) -> gate array pin 19 | |
| 5 | wire B: VIA socket pin 39 (SOE) -> gate array pin 39 | frees GPIO3 |
| 6 | wire C: Pico pin 5 (GP3) -> gate array pin 31 | flux pulses, 3.3 V push-pull (no level shifter) |

The buzzer stays. **After the mod the old firmware must not run**: the 74LVC4245 then always drives
GPIO8-15, and the old firmware drives them too.

## Firmware status (milestone 1, `FLUX_MODE`, on by default)

Done in `firmware/src/flux.c` (math in `include/flux_math.h`, host test `tests/flux_math_test.c`):

* the raw track bytes go by DMA (two channels chained in a circle, re-armed once per revolution)
  to a PIO state machine: one bitcell = 64 PIO cycles, a pulse on GPIO3 for every 1-bit
* PIO clock divider from the track length: a revolution is 200 ms, the angle is global and kept
  on head steps; the pulse keeps its width in ns on every track
* pulses only in read mode: the PIO checks MODE (GPIO1) with `jmp pin` for every bit
* calibration at start (display "Flux calibrate.."): a test pattern with syncs is played with
  active-low/high pulses of 100-1500 ns; the gate array's SYNC (GPIO2) shows which work, the middle
  of the working range is used. No answer: GPIO3 stays low and nothing is played
* a second state machine takes GPIO1..16 at every falling BYTE READY (MODE, SYNC, port A) into a DMA
  ring: read bytes feed the sector display, write bytes go into the track at their bit position
  (the first BYTE READY of a write is timed by a GPIO interrupt)
* SYNC, BYTE READY and port A can not be driven by the Pico in this build (the macros are build errors)
* `.den` rules are not applied: the gate array decodes at the real density
* USB log: calibration table (printed again when a terminal connects), track changes, motor,
  2-second statistics (bytes read, headers, bytes written, lost)

Not yet: random flux in no-flux areas (weak bits) - RapidLok (Law of the West) and V-Max will
probably still fail; per-byte speed maps; a menu entry to repeat the calibration.

## Phase 2 - firmware: flux output

* build option `FLUX_MODE`: GPIO8-15 always inputs (the 74LVC4245 drives them), BRDY and SYNC never
  pulled (read only), GPIO3 = flux output
* tracks as flux: G64 bits -> a reversal at every 1 bit, cell = the track's speed zone
  (4.00/3.75/3.50/3.25 us) or the per-byte speed map; D64 tracks via GCR as now
* PIO program on GPIO3: pulse (polarity/width from phase 0), wait N PIO cycles, next; fed by DMA
  from a buffer of intervals. 150 MHz PIO clock = 6.7 ns resolution, one revolution = 200 ms
* head step: switch to the new track's buffer at the same angle; motor off: stop pulses
* weak/no-flux areas: a real drive's amplifier turns them into random reversals; the PIO feed adds
  random intervals there (RapidLok and V-Max check that such areas read differently each time)

## Phase 3 - firmware: write capture

* the drive writes bytes: the VIA puts them on port A and the gate array shifts them out, pulsing
  BYTE READY for each. The Pico reads port A at every BYTE READY from the gate array, as the current
  firmware already does with its own pulses
* the time between BYTE READY pulses gives the density written at (26/28/30/32 us per byte)
* bits go into the track buffer at the current angle and are saved to G64/D64 as now

## Phase 4 - tests

1. normal D64 load/save/format, all zones
2. Bounty Bob (half-tracks), Law of the West (no `.den`), Defender of the Crown (V-Max)
3. logic analyser: BYTE READY spacing from the gate array, pulses on U10 pin 33
4. host tests: flux generation from G64 checked against the VICE read model (`gcr_resample`)

## Phase 5 - new PCB version

* flux out and write in with proper 5 V buffers, MODE/SOE switchable on the board, a connector for
  the two motherboard wires
* 1541-I long/short: find the read pulse input (UF4 CLR / UE7 load in the discrete circuit) and the
  write data output on those schematics

---

## Fork and name

1541-FluxPico, forked from fook42/1541-rePico (GPL-3.0); see README.
