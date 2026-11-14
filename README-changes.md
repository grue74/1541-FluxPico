# 1541-FluxPico – changes from 1541-rePico: bit-level G64 emulation and more

This document describes a set of changes to the firmware (not yet committed):

1. **Bit-level G64 emulation**: copy-protected G64 images are fed to the drive the way a real 1541 reads them
2. **Half-track support** (e.g. *Bounty Bob Strikes Back*)
3. **G64 loader and G64 save fixes**
4. **Track/sector display** while the drive works
5. **Sorted directory listing** in the file browser and the C64 selector
6. **Beeper on/off** in the Settings menu
7. **Fixes from the first hardware tests** (stepper input, data port with SOE off, selector head alignment)
8. **Density rules per image** (`<image>.den`) for loaders that read tracks at another density
9. **Debug log on USB serial** (build option `DEBUG_DRIVE_LOG`)

Status: tested on the PC (host tests, see below) and on the hardware (PCB v17): *Bounty Bob Strikes
Back* loads. *Law of the West* needs a `.den` file (section 8); the hardware test of that is pending.
See the [hardware test list](#hardware-test-list).

---

## 1. Bit-level G64 emulation

### Why

The firmware used to send the bytes of a G64 track to the 1541 exactly as they are stored in the file,
one byte every 26/28/30/32 µs. A G64 track is really a **bit stream**: the 1541 read circuit
restarts its bit counter after every SYNC (10 or more 1-bits), so the bytes the drive sees are not
necessarily aligned to the bytes in the file. Normal DOS disks happen to be aligned, but many copy
protections are not, and they also rely on other effects of the read circuit, such as the timing
of the track or runs of 0-bits. So those images did not work.

### How it works now

* **Resolver** (`src/gcr_resolve.c`). When the head arrives on a track, the main loop
  simulates the 1541 read circuit for one revolution. The model is taken from VICE
  (`src/drive/rotation.c`, `rotation_1541_gcr()`):
  * 10-bit shifter: 10 one-bits assert SYNC, and the bit counter is held at 0 while SYNC is active
  * the first 0-bit ends the sync and is bit 1 of the next byte; every 8th bit gives BYTE READY
  * after three 0-bits the counter in the read circuit wraps and reads a 1 (`1 0000` is read as
    `1 0001`)
  * longer runs without flux reversal give random bits (**weak bits**). They are randomized every
    time they are read, because protections read twice and compare.

  Only the **framing** is stored: the bit positions where SYNC starts and ends. Byte values are
  taken from the raw track at the right bit position when they are played, so writes are visible
  at once and the framing needs only about 8 KB per buffer (double-buffered).
* **Playback** (`src/drive.c`). The fixed byte timer is replaced by an event-driven alarm on a
  global **disk angle clock**:
  * one revolution is 200 ms (300 rpm), and one bitcell is 200 ms / (8 × track length), like in VICE
  * SYNC and BYTE READY are output at their exact bit positions (1 µs timer resolution, no drift)
  * the angle is kept when the head steps, as on a real disk. Before, every track started at byte 0.
  * between events the OE/motor lines are checked at least every 8 bitcells
* **Writes** are bit-exact and use the drive's current byte framing. When the drive switches back
  to reading, the track is resolved again. Until the new framing is ready (a few ms), only the
  area that was just written is muted, so writing sectors back to back keeps working.

Normal D64/PRG/selector images also run through the new path. Their byte timing is unchanged
within 0.3 % (26.02 / 28.07 / 30.03 / 32.06 µs).

## 2. Half-tracks

* Half-tracks (1.5, 2.5, …) stored in a G64 are loaded into an **80 KB pool** at their real length.
  Images without half-tracks use none of it. If the pool is full, the remaining half-tracks read as
  the track below, which is what the firmware always did for half-track positions.
* The head position follows **every stepper phase immediately**. Before, the firmware waited 5 ms
  after the last step and rounded down to a whole track. Protections such as Bounty Bob step half a
  track at a time and expect the data to continue at the same angle. The 5 ms timer now only
  switches the buzzer off.
* On a half-track position without stored data, the head reads the track below (as before), so
  normal disks behave exactly as before.

## 3. G64 loader and G64 save fixes

* The number of half-tracks is taken from the header, so the speed table is read from the right
  position (it was wrong for files without 84 entries).
* Tracks longer than 7928 bytes are truncated. Before, they overflowed the track memory.
* Missing tracks and tracks with length 0 become unformatted tracks (`0x55`, nominal length), as in
  VICE. Before, the length could be 0 and playback ran through memory.
* **Save Image (G64)** writes a fresh, sequential layout including half-tracks. The old code reused
  the offsets of the loaded file and wrote the wrong data for images with half-tracks.
* Speed zone maps (speed values ≥ 4) are not used for timing; the timing comes from the track length.

## 4. Track/sector display

First display line in the info and selector screens:

```
T:18+S:07W WProt
```

* `T:18` is the track under the head; `+` means the head is on a half-track (18.5)
* `S:07` (motor on) is the sector of the last header that passed under the head. `S:--` means no
  header was seen in the last 100 ms (unformatted or protection track). The field is blank when the
  motor is off.
* `W` means the shown sector is being written; it is held for 0.5 s after the write
* the old `Motor` text is gone, since a shown sector already means the motor is running
* refreshed every 150 ms (not 200 ms, which would always sample the same angle)

The Pico sees every header that passes but cannot tell which sector the 1541's CPU actually wants.
While reading, `S:` therefore counts through the sectors under the head. Writes are exact.

## 5. Sorted directory listing

* Directories are read once when entered and sorted: **folders first**, then files,
  **case-insensitive**, **numbers by value** (`disk 2` before `disk 10`), and the name before the
  extension (`Archon.d64` before `Archon II.d64`). `..` stays on top in subfolders.
* The LCD file browser, the C64 selector menu and the "next image" key all use the same order, so
  a number selected on the C64 opens the file shown there. Scrolling no longer rescans the card.
* Up to 1024 entries / 32 KB of names. Larger folders are shown unsorted in card order, as before,
  and nothing is lost.
* When a file is opened, its size and read-only flag are read from the card (`f_stat`).

## 6. Beeper on/off

* New entry in the Settings menu: **`Beeper      On`/`Off`**. It switches the stepper sound of the
  buzzer. Switching it off silences a sound that is already playing.
* Only on PCB v17 and later (PCB v16 has no buzzer, so the entry is not shown there).
* The default is **On**, as before. The setting is **stored in flash** (`src/settings.c`): last 4 KB
  sector, magic + version + checksum, written only when a value changes. Flashing a new UF2 keeps it.
  The save runs on core0 with `flash_safe_execute()` (core1 is parked for about 50 ms; the disk
  emulation resumes at the right angle).

## 7. Fixes from the first hardware tests

* **Stepper input.** A step can change both phase lines at once, and through the level shifters a
  rising edge is slower than a falling one. The GPIO interrupt read the lines at the first edge and a
  2 ms filter dropped the second one, so the head position could end up one track off (Bounty Bob
  loaded from the wrong track). Now every edge restarts a 20 µs timer and the phase is read when the
  lines are stable (`STEP_SETTLE_TIME`); the 2 ms filter is gone.
* **Head position follows the phase absolutely.** Like the stepper motor, a position belongs to one
  phase: `phase = (position + 2) & 3` (whole tracks have even phases, track 18 has phase 0, as in
  the 1541 and VICE). A new phase moves the head to the neighbour with that phase; at the end stop
  or with the opposite phase it stays (bump). Before, only phase changes were counted from a start
  value taken at power up, so the head could sit half a track off for the whole session: DOS still
  worked (a missing half-track plays the track below) but half-track loaders failed.
* **Data port with SOE off.** On a 1541 the read shift register always feeds the VIA port; SOE only
  gates BYTE READY. The firmware put a byte on the port only with SOE on. RapidLok polls the port
  with SOE off. Now every byte (and `$FF` during a sync) is put on the port; BYTE READY still needs
  SOE. The bus transceiver direction follows the read/write line, not SOE, so this is safe.
* **Selector head alignment.** Mounting the C64 selector (or a text image) set the head to track 18
  regardless of the stepper phase. It now goes to the position near track 18 that belongs to the
  current phase (`set_head_near_track()`, also used at power up).
* **Track framing hand-over** to the playback interrupt uses a compiler barrier.
* **Interrupt latency.** The playback alarm interrupt has the highest priority on core0 (USB and
  GPIO interrupts had the same priority and could delay it). When the interrupt still runs late
  and catches up, a BYTE READY pulse waits until the previous one is at least 16 µs ago
  (`BYTE_MIN_GAP_US`); the playback itself stays on time. Two pulses back to back were seen as one
  byte by the drive's `BVC` loops, which shifted a whole loader segment (Bounty Bob failed now and
  then with a checksum error).
* **USB serial** works again: the test builds had been made without the TinyUSB submodule.

## 8. Density rules per image

The board does not see the density select lines (VIA2 PB5/PB6). Some loaders read a track with a
density that does not match the rate it was recorded at; a real drive then decodes other bytes.
RapidLok (*Law of the West*) reads its key track 36 at density 3 and needs at least 256 bytes between
its two syncs: at the recorded rate there are 254, read at density 3 about 270.

A text file next to a G64 with the same name and the extension `.den` lists such tracks:

```
# Law of the West (Accolade 1985), RapidLok 1
32-36 3
```

`first[-last] density` per line (tracks 1–42, density 0–3, 3 = tracks 1–17), `#` starts a comment.
When the image is opened these tracks are converted once into the bit stream the drive decodes at
that density (`gcr_resample()`, an event based port of the VICE read circuit model; about 0.1 s per
track on the Pico). The file browser does not list `.den` files. Half-tracks are not converted.
*Save Image (G64)* stores the converted tracks (speed 3). A track whose G64 speed already equals the
rule's density is not converted, so loading such a saved image with its `.den` does not convert it
twice. A rule changes only the density a track is played at: the D64 layout (sectors per track, gaps,
interleave for D64 load/save, conversion and the selector) always follows the track number.

The gaps get their bits from random flux reversals, and the conversion keeps one fixed result
(`GCR_RESAMPLE_SEED`). The host test converts track 36 of both PAL sides with 200 other seeds: the key
run is 268–272 bytes every time (the fixed seed gives 269, 256 are needed), and the seam of the
conversion is never inside it.

The converted track keeps the gaps without flux as gaps: the playback creates the phantom and
random bits there on every revolution, as for any other track (RapidLok checks that its header gaps
read differently each time). The gap model itself (`gcr_fetch_byte`) now uses the bit probabilities
of the VICE read circuit per density instead of a flat 25 %: the first random flux reversal comes
18–20 µs after the last real one, so the first bits of a gap are nearly fixed (RapidLok header bytes
end there). VICE loads a converted *Law of the West* image into the game; the VICE comparison over
181 G64 images gives the same result as before.

`test_disks/` has `.den` files for both PAL sides of *Law of the West*. The rule depends on the
loader: derive it from the loader code or from VICE. Connecting the density lines to the Pico
(hardware change plus firmware) is described in `doc/density-select-mod.pdf`.

Note: *Law of the West* does not load with the S-JiffyDOS drive ROM at all (also not in VICE):
RapidLok's decryption depends on scratch bytes of the stock DOS GCR decoder.

## 9. Debug log on USB serial

Build with `target_compile_definitions(1541-rePico PRIVATE DEBUG_DRIVE_LOG)`; connect the Pico's USB
port (drive switched on first) and open the COM port with any speed. The disk interrupt only writes
small records into a ring buffer; the main loop prints them:
`HEAD`, `READY` (track analysed), `IDLE`, `SYNC` (first two after a step), `REV` (per revolution, only
when it changes), `LATE`, `WRITE`, `STEP?`, `MOTOR`, `SOE` (on-periods of 1 ms or more = DOS jobs),
`BLOCK` (data block read), `HDR` (bytes after header syncs when a loader starts right after a job).

---

## Behaviour changes at a glance

| Before | Now |
|---|---|
| G64 bytes sent as stored | 1541 read circuit simulated (framing, phantom 1s, weak bits) |
| Byte timer 26/28/30/32 µs per zone | Bit timing from the track length, 200 ms per revolution |
| Every track starts at byte 0 after a step | Angle kept across steps |
| Head position updated 5 ms after stepping, whole tracks only | Updated with every phase, half-tracks |
| Half-tracks in G64 ignored | Loaded (80 KB pool) |
| Track > 7928 bytes overflowed memory | Truncated |
| Missing track: undefined length | Unformatted `0x55` track |
| Save G64 reused old offsets | New layout incl. half-tracks |
| `T:18 Motor WProt` | `T:18+S:07W WProt` |
| Directory in FAT order | Sorted (folders first, natural order) |
| Beeper always active (PCB v17) | Settings → Beeper On/Off |

## Known limitations

* **Density select lines are not connected to the Pico.** The decoder clock is assumed to match the
  density of the track. Loaders that read a track at another density (RapidLok: key track 36 at
  density 3; A View To A Kill tracks 37–41, Rally Speedway track 36) get the bytes of the recorded
  rate. Workaround: a `.den` file per image (section 8). Proper fix: hardware change plus firmware,
  see `doc/density-select-mod.pdf`.
* Weak bits are random, but the number of bits read stays fixed.
* The track number on the display is still 2 digits; a half-track is shown with `+`.

## RAM

Pico 2, 512 KB RAM (measured with `--print-memory-usage`):

| Block | Size |
|---|---|
| Whole tracks (42 × 7928) | 333 KB (unchanged) |
| Half-track pool | 80 KB (new) |
| Sorted directory list | 34 KB (new) |
| Framing buffers (2 × 2048 syncs) | 16 KB (new) |
| Rest (FatFS, display, buffers, SDK) | about 24 KB |
| **Free for heap** | **about 38 KB** |

Flash: about 168 KB of 4 MB.

A single variable-length pool for whole and half-tracks would save about 33 KB and make the space
flexible. It needs rework of code that treats the track memory as one block (menu builder, D64
save). Not done.

---

## Files

New:

| File | Purpose |
|---|---|
| `firmware/src/gcr_resolve.c`, `firmware/include/gcr_resolve.h` | read circuit model, framing, playback cursor, bit-level read/write |
| `firmware/src/dirlist.c`, `firmware/include/dirlist.h` | sorted directory listing |
| `firmware/tests/` | host tests (see below) |

Changed:

| File | Change |
|---|---|
| `firmware/src/drive.c`, `firmware/include/drive.h` | event-driven playback, writes, stepping, header capture for the display |
| `firmware/src/rw_routines.c` | G64 loader fixes, half-track loading, new G64 save |
| `firmware/src/globals.c`, `firmware/include/globals.h` | half-track pool, nominal track sizes |
| `firmware/src/main.c`, `firmware/include/main.h` | main loop resolves tracks; prototypes |
| `firmware/src/gui.c`, `firmware/include/gui_constants.h` | track/sector display |
| `firmware/src/files.c`, `firmware/include/files.h` | sorted listing in browser and selector; `f_stat` on open |
| `firmware/src/menu_image.c` | selector menu in sorted order; clears half-tracks |
| `firmware/src/main.c`, `firmware/include/main.h`, `firmware/include/mymenu.h`, `firmware/src/gui.c` | Beeper On/Off setting |

Optional debug output: add `target_compile_definitions(1541-rePico PRIVATE DEBUG_RESOLVE_TIME)`
to `firmware/CMakeLists.txt` and the resolve time of every track is printed on USB serial.

## Tests

Host tests run on the PC (not part of the firmware build):

```
cmake -S firmware/tests -B build-tests
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release
```

* **`gcr_resolve_test [file.g64 ...]`**. Run on 183 G64 images (6670 tracks):
  * the framing matches a straight per-bit simulation
  * the framing does not change when a track is rotated
  * 400 random seek positions per track are correct
  * compared with a port of the VICE read circuit model, the bytes after every sync match, except in
    weak-bit areas (random in both) and on the off-density tracks named above
  * resolve time on the PC: 25 µs average per track; about 2 ms estimated on the Pico
* **`drive_harness [file.g64]`** runs `drive.c` with a fake timer and recorded GPIOs:
  * DOS tracks in all zones decode, with the correct byte timing
  * a DOS-style sector write is read back by `convert_gcr2d64track` (the selector path)
  * two sectors written back to back
  * switching to write mode inside a sync; stepping keeps the angle; an empty track idles
  * header/write capture for the display
  * with `bbsb.g64` (Bounty Bob): the firmware loader reads 10 half-tracks, stepping
    1.0 → 1.5 → 2.0 → 2.5 plays each track at the correct angle, and G64 save + reload keeps
    everything
  * with a RapidLok image as second argument (`drive_harness bbsb.g64 lotw_s0.g64`): density rules,
    a seed sweep of the track 36 conversion, playback of the converted track, the D64 layout of
    converted tracks, and save + reload with the `.den` (no second conversion)
* **`dirlist_test`**: sort order, fallback for large folders, path check.

The file `gcr_resolve_test.c` contains a port of the VICE read loop (GPL-2.0-or-later,
compatible with this project's GPLv3). The authors are credited in the file.

## Hardware test list

Test firmware (not in git, see `.gitignore`):

* `firmware/build-hwtest/1541-rePico_pcb17.uf2` for PCB v17 (default)
* `firmware/build-hwtest/1541-rePico_pcb16.uf2` for PCB v16 (legacy)

These were built locally with the Arm GNU Toolchain 14.2.1 and pico-sdk 2.3.1 (CI uses the newest
SDK). The C64 programs come from the committed `.prg` files: assembling `menu.asm` at the current
source gives the same output as for the committed binaries.

Please check:

1. D64, PRG and the C64 selector (load, select, "next image")
2. SAVE a file, then VERIFY
3. FORMAT (`N:` command)
4. Save Image (G64 and D64)
5. A fastloader that steps quickly
6. Weak bits: *Silent Service* (RapidLok 6)
7. Off-zone density: *Stunt Car Racer*
8. Half-tracks: *Bounty Bob Strikes Back* (`bbsb.g64` from kryoflux.com). Set **WriteProt On**
   first: the protection aborts with "CHECKSUM ERROR! LOAD ABORTED!" on a writable disk
   (also in VICE 3.10; with a read-only disk VICE loads it)
9. Display: `S:` counts while the motor runs, `W` during writes, `+` on half-tracks
10. File browser and selector: sorted order, and selecting by number opens the right file
11. Settings → Beeper Off: no stepper sound; On: sound again (PCB v17); the setting survives a power cycle
12. Density rules: *Law of the West* PAL side 0 with its `.den` file (stock or JiffyDOS drive ROM,
    not S-JiffyDOS): the RapidLok loader steps to track 36 and back and the game starts.
    With `DEBUG_DRIVE_LOG`, check: `density rules: track 32..36 ... -> ~7160 bytes` when the image
    opens (missing = `.den` not found); `SYNC 36` with about 269 bytes since the last sync; no `LATE`
    or `IDLE` on track 36. If it fails, also try the image saved with *Save Image (G64)* without a
    `.den` (already converted): that separates file handling from playback
