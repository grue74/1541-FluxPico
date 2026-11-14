# Plan: bit-stream read circuit in the Pico (plug-in version)

Goal: the board stays a plug-in for the VIA socket, but the Pico no longer plays bytes. It holds each
track as a **flux stream** (recorded at the track's own rate) and runs the **1541 read circuit** on it at
the **density the drive selects right now** (DS0/DS1). The output (data on PA, SYNC, BYTE READY) is
timed by a **PIO state machine**, not by a timer interrupt.

What this removes: `.den` files, the weak-bit probability table, the byte resolver, the alarm-driven
playback and its interrupt-latency problems (Bounty Bob checksum errors, the 233-byte track 36).

---

## Phase 0 - hardware mod on the current PCB v1.7

The mod is the one in `doc/density-select-mod.pdf` (generator: `tools/density_pdf/`). Nothing else is
needed for the test version.

A 4-channel BSS138 level shifter module (HV1-4/LV1-4, HV, LV, GND, 10k pull-ups on both sides)
converts the two lines; two channels are used. It sits on the top side where the buzzer was.

| Module pin | Wire to |
|---|---|
| HV1 / HV2 | VIA socket pin 15 (DS0) / pin 16 (DS1), bottom side |
| HV | VIA socket pin 20 (+5 V) |
| LV | J8 pin 1 (3.3 V) |
| LV1 | J8 pin 2 = GPIO17 (buzzer removed) |
| LV2 | Pico pin 5 = GPIO3 (SOE_GA trace cut) |
| GND | J8 pin 3 |

SOE_GA: cut the trace Pico pin 5 -> R19 and wire R19 pad 2 to GND (keeps the 1541-II gate array as before).

**Flash the mod firmware first.** The old firmware drives GPIO3 and GPIO17 as outputs; through the
bidirectional shifter that pulls DS0/DS1 low and the drive reads at the wrong density.

Checks after the mod: see the PDF, step 5.

Optional for debugging: a logic analyser on BRDY (GPIO22), SYNC (GPIO2), PA0 (GPIO8), DS0, DS1, SOE (GPIO0).

## Phase 1 - read DS0/DS1 (small, testable on its own)

* build option `DENSITY_PINS` (PCB v1.7 + mod): GPIO_DS0 = 17, GPIO_DS1 = 3, inputs without pulls;
  no buzzer, no SOE_GA output in that build; hide the Beeper menu entry
* show the density in the debug log (and optionally the display)
* **Test:** with a normal D64, the log must show density 3/2/1/0 on tracks 1-17/18-24/25-30/31-35.
  RapidLok must show density 3 when it is on track 36.

## Phase 2 - tracks as flux streams

* when a track is loaded: G64 bits -> flux times, a flux reversal at every 1 bit; bit cell = the
  track's speed zone (4.00/3.75/3.50/3.25 us), or the per-byte speed map if the G64 has one
* times in units of 1/16 us (the 1541's 16 MHz clock), one revolution about 200 ms
* D64 tracks are first turned into GCR as now, then into flux the same way
* the angle is kept when the head steps (as now)

## Phase 3 - the read circuit model (core0, ahead of the output)

* port of the VICE `rotation_1541_gcr` model, already written event based as `gcr_resample()`:
  UE7 counter (density 0-3), UF4, the filter's random delay, 10-bit sync detector, 8-bit shift register
* runs at the live density; output is a list of events for the PIO:
  `(time to wait, PA byte, SYNC level, BYTE READY pulse yes/no)`
* no-flux areas give random bits by themselves (the filter's random delay), as in VICE, so the
  `gap_p` table goes away
* computes about 0.5-1 ms ahead. When the density changes, the head steps or the motor stops, the
  queued events are dropped and the model restarts at the current angle (one byte of latency at most,
  like a real drive's counters restarting)

## Phase 4 - PIO output

* pins: SYNC = GPIO2, PA0-7 = GPIO8-15, BRDY = GPIO22. One `out pins` with base 2 and count 21 sets
  them all at once; the pins in between (3-7, 16-21) are not given to the PIO, so they are not touched
* one FIFO word per event: wait count + pin values, fed by DMA from the event list
* BYTE READY is pulsed only while SOE (GPIO0) is on: `jmp pin` inside the PIO program, so it follows
  SOE without the CPU
* PA direction: as now, the transceiver follows the read/write line; in write mode the PIO stops
  driving PA
* result: the gap between BYTE READY pulses is exact to the PIO clock, whatever the CPU is doing

## Phase 5 - writing

* keep the current byte capture in write mode (the VIA writes bytes, the Pico stores bits)
* new: the written bits get the density read from DS0/DS1, so a track written at another density is
  stored at its real rate (G64 speed zone / speed map)

## Phase 6 - clean up and host tests

* remove: `.den` handling (`apply_density_rules`, `.den` hiding in dirlist), `gap_p`, the resolver,
  the alarm playback and `BYTE_MIN_GAP_US`
* host harness (`firmware/tests/`): run the model + the event list and check
  * normal D64: every sector decodes, at all four zones
  * Bounty Bob half-tracks
  * Law of the West track 36 read at density 3: 256+ bytes between the two syncs on every revolution,
    **without** a `.den` file; compare with VICE
  * V-Max (Defender of the Crown)
  * timing: no two BYTE READY events less than one byte time apart
* run the event output of the harness against the same images in VICE (`--density-vice` mode) to check
  the bytes match

## Phase 7 - hardware tests on the modded board

1. normal D64 load + save, `N:` format, verify with a copier
2. Bounty Bob (several loads)
3. Law of the West side 0 and 1, PAL and NTSC images, without `.den`
4. Defender of the Crown (V-Max)
5. the old test list in `README-changes.md`
6. logic analyser: BYTE READY spacing 26/28/30/32 us per zone, no doubles

## Phase 8 - new PCB version

* DS0/DS1 routed to the Pico on the board (with the divider), the buzzer moved to a free GPIO or I2C
* SOE_GA tied low on the board (or dropped)
* everything else stays plug-in: same VIA socket, same connectors

---

## Fork and new name

The project is GPL-3.0 (`LICENSE`), so a fork must stay GPL-3.0, keep the copyright notices and say it
is based on 1541-rePico with a link to the original repository.

Steps (not done yet, they need your GitHub account):
1. commit the current work on a branch here (`bitstream-base`), so nothing is lost
2. fork `fook42/1541-rePico` on GitHub, rename the fork, push the branch
3. new README: what changed, credit to the original project, the mod PDF
4. rename the UF2 output, the CMake project name and the name shown on the display/selector

Chosen name: **1541-FluxPico**. Other ideas were:

* **1541-rePico-Flux**: makes the origin obvious
* **BitPico 1541**
