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

## Phase 0 - measure first (no changes)

On the unmodified drive with the **original VIA** (or the board with the old firmware), with a real
disk and a logic analyser or scope:

1. **U10 pin 33 (CLR)** while loading a directory: pulse polarity (active low or high), width, and
   level. This sets how the Pico has to pulse.
2. **U10 pin 40 (B)** while formatting a scratch disk: confirm it toggles per flux reversal (bit
   cells of 3.25-4 us; gaps of 1, 2 or 3 cells).
3. U10 pin 19 and pin 41 levels during a read job, to confirm MODE/SOE as on the schematic.

## Phase 1 - modification (PCB v1.7 + 1541-II)

Everything switchable, so the old firmware can still be used during development.

| # | What | Why |
|---|---|---|
| 1 | **MODE jumper.** Lift P1 pin 19 from the board's GND (isolate the pad or replace the pin with a wired one) and add a 3-pin jumper: P1.19 to GND (old firmware) or to IC1 pin 19 = MODE (flux mode). | the gate array must see read/write mode to drive PA and take bytes for writing |
| 2 | **SOE jumper.** Cut the SOE_GA trace between Pico pin 5 and R19 (as in `density-select-mod.pdf`). Jumper: P2.19 (SOE_GA) to Q9/R19 (old firmware) or to IC1 pin 39 = SOE (flux mode). Do not ground R19 pad 2 in flux mode. | the gate array needs the real SOE with no delay; frees GPIO3 |
| 3 | **Cut U12 pin 33 -> U10 pin 33** on the drive's main board, close to U12. | the hybrid would fight the Pico's pulses |
| 4 | **Flux out:** Pico GPIO17 (J8.2, buzzer removed) -> level shifter -> **U10 pin 33** | the pulses |
| 5 | **Write in:** **U10 pin 40** -> level shifter -> Pico GPIO3 (Pico pin 5) | to store what the drive writes |
| 6 | Level shifter: the 4-channel BSS138 module, HV = 5 V (VIA pin 20), LV = 3.3 V (J8.1), GND (J8.3). If phase 0 shows short active-high pulses, use a 74AHCT1G125 (5 V) for the flux line instead: the BSS138 module drives low edges fast but rises slowly through 10k. | |
| 7 | Unplug the head cable (or keep no disk in the drive): in write mode the hybrid would write to a real disk. | safety |

**Never run the old firmware in flux mode.** The gate array then drives PA while the Pico's
74LVC4245 also drives it. The jumpers in old-firmware position restore the old wiring.

## Phase 2 - firmware: flux output

* build option `FLUX_MODE`: transceiver disabled (GPIO1 OE), BRDY and SYNC never pulled, PA read
  only (for the sector display), no buzzer, GPIO3 input
* tracks as flux: G64 bits -> a reversal at every 1 bit, cell = the track's speed zone
  (4.00/3.75/3.50/3.25 us) or the per-byte speed map; D64 tracks via GCR as now
* PIO program on GPIO17: pulse (polarity/width from phase 0), wait N PIO cycles, next; fed by DMA
  from a buffer of intervals. 150 MHz PIO clock = 6.7 ns resolution, one revolution = 200 ms
* head step: switch to the new track's buffer at the same angle; motor off: stop pulses
* weak/no-flux areas: a real drive's amplifier turns them into random reversals; the PIO feed adds
  random intervals there (RapidLok and V-Max check that such areas read differently each time)

## Phase 3 - firmware: write capture

* PIO on GPIO3 timestamps every edge of U10 pin 40 while MODE is write
* intervals / bit cell (from the current zone, or measured from the 1-cell intervals) -> GCR bits ->
  into the track buffer at the current angle; saved to G64/D64 as now

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

Superseded: `doc/density-select-mod.pdf` describes reading DS0/DS1 into the Pico. In flux mode the
gate array gets DS0/DS1 directly; only its SOE_GA cut is reused.
