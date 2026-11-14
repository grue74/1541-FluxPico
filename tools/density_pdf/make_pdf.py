"""Build doc/density-select-mod.pdf: instructions for connecting the density
select lines (VIA2 PB5/PB6) to the Pico (option 1) and the firmware work
that uses them (option 2). Board drawings are made from the KiCad PCB file.
"""
import os

from reportlab.graphics.shapes import Circle, Drawing, Line, PolyLine, Polygon, Rect, String
from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.platypus import (Image, KeepTogether, PageBreak, Paragraph, SimpleDocTemplate, Spacer, Table,
                                TableStyle)

from pcb import Board

REPO = r'E:\GitHub\1541-rePico'
OUT = os.environ.get('PDF_OUT', os.path.join(REPO, 'doc', 'density-select-mod.pdf'))
PHOTO_PCB = os.path.join(REPO, 'doc', 'pictures', 'setup', '00.PCB_Version17.jpg')
PHOTO_BUZ = os.path.join(REPO, 'doc', 'pictures', 'setup', '05.buzzer_cap_top.jpg')

B = Board()

# colours
C_BOARD = colors.HexColor('#E8F2E8')
C_EDGE = colors.HexColor('#2E6B2E')
C_PAD = colors.HexColor('#B9B9B9')
C_TRACK = colors.HexColor('#C9D9C9')
C_CUT = colors.HexColor('#D62828')
C_DS0 = colors.HexColor('#1F6FD1')
C_DS1 = colors.HexColor('#E07B00')
C_GND = colors.HexColor('#444444')
C_SOE = colors.HexColor('#D62828')
C_TEXT = colors.HexColor('#1A1A1A')

styles = getSampleStyleSheet()
H1 = ParagraphStyle('H1', parent=styles['Heading1'], fontSize=16, spaceAfter=6, textColor=C_EDGE)
H2 = ParagraphStyle('H2', parent=styles['Heading2'], fontSize=12.5, spaceBefore=8, spaceAfter=4, textColor=C_EDGE)
H3 = ParagraphStyle('H3', parent=styles['Heading3'], fontSize=10.5, spaceBefore=6, spaceAfter=2)
BODY = ParagraphStyle('B', parent=styles['BodyText'], fontSize=9.5, leading=12.5, spaceAfter=4)
SMALL = ParagraphStyle('S', parent=BODY, fontSize=8.3, leading=10.5, textColor=colors.HexColor('#333333'))
CAP = ParagraphStyle('C', parent=SMALL, alignment=TA_CENTER, spaceBefore=2, spaceAfter=8)
NOTE = ParagraphStyle('N', parent=BODY, backColor=colors.HexColor('#FFF4E0'), borderPadding=5,
                      borderColor=colors.HexColor('#E0B060'), borderWidth=0.6, spaceBefore=4, spaceAfter=8)
WARN = ParagraphStyle('W', parent=NOTE, backColor=colors.HexColor('#FDE8E8'), borderColor=colors.HexColor('#D62828'))


def P(text, st=BODY):
    return Paragraph(text, st)


def table(rows, widths, header=True, size=8.5):
    data = [[Paragraph(str(c), ParagraphStyle('t', parent=SMALL, fontSize=size, leading=size + 2)) for c in r]
            for r in rows]
    t = Table(data, colWidths=widths, hAlign='LEFT')
    st = [('GRID', (0, 0), (-1, -1), 0.4, colors.HexColor('#999999')),
          ('VALIGN', (0, 0), (-1, -1), 'TOP'),
          ('LEFTPADDING', (0, 0), (-1, -1), 3), ('RIGHTPADDING', (0, 0), (-1, -1), 3)]
    if header:
        st.append(('BACKGROUND', (0, 0), (-1, 0), colors.HexColor('#DDEBDD')))
    t.setStyle(TableStyle(st))
    return t


from figs import fig_bottom_full, fig_bottom_zoom, fig_top_buzzer, resistor  # noqa: E402


# --- schematic of the modification -----------------------------------------

def fig_schematic():
    W, H = 165 * mm, 118
    d = Drawing(W, H)

    def box(x, y, w, h, lines, size=7.3):
        d.add(Rect(x, y, w, h, fillColor=colors.white, strokeColor=C_TEXT, strokeWidth=1))
        top = y + h - 11
        for i, t in enumerate(lines):
            d.add(String(x + w / 2, top - i * 9.2, t, fontName='Helvetica-Bold' if i == 0 else 'Helvetica',
                         fontSize=size, fillColor=C_TEXT, textAnchor='middle'))

    def gnd(x, y):
        d.add(Line(x, y, x, y - 4, strokeColor=C_GND, strokeWidth=1))
        for i, wdt in enumerate((10, 6, 2)):
            d.add(Line(x - wdt / 2, y - 4 - i * 2.2, x + wdt / 2, y - 4 - i * 2.2, strokeColor=C_GND, strokeWidth=1))

    vx, vw = 4, 92
    px, pw = W - 102, 98
    box(vx, 26, vw, 88, ['VIA2 6522 (drive)', '', 'PB5 = DS0, pin 15', '', 'PB6 = DS1, pin 16'])
    box(px, 26, pw, 88, ['Raspberry Pi Pico 2', '', 'GPIO17 (pin 22)', '= J8 pin 2, was buzzer', '',
                         'GPIO3 (pin 5)', 'was SOE_GA'])
    for yy, col, name, gp, frac in ((92, C_DS0, 'DS0', 'GPIO17', 0.58), (52, C_DS1, 'DS1', 'GPIO3', 0.74)):
        x0, x1 = vx + vw, px
        node = x0 + (x1 - x0) * frac
        resistor(d, x0, yy, node - 30, yy, col, '10k')
        d.add(Line(node - 30, yy, x1, yy, strokeColor=col, strokeWidth=1.4))
        d.add(Circle(node, yy, 2.2, fillColor=col, strokeColor=None))
        resistor(d, node, yy, node, yy - 22, col, '22k')
        gnd(node, yy - 22)
        d.add(String(x0 + 4, yy + 4, name, fontName='Helvetica-Bold', fontSize=7.5, fillColor=col))
        d.add(String(x1 - 4, yy + 4, gp, fontName='Helvetica-Bold', fontSize=7.5, fillColor=col, textAnchor='end'))
    d.add(String(4, 12, 'SOE_GA: the trace from Pico pin 5 to R19/Q9 is cut, R19 pad 2 (the R19/Q9 side) is wired '
                        'to GND.', fontName='Helvetica', fontSize=7.3, fillColor=C_SOE))
    d.add(String(4, 2, 'The firmware always drove SOE_GA low, so the 1541-II gate array sees the same level as '
                       'before.', fontName='Helvetica', fontSize=7.3, fillColor=C_SOE))
    return d


# --- option 2: firmware flow -------------------------------------------------

def fig_flow():
    W, H = 165 * mm, 70 * mm
    d = Drawing(W, H)

    def box(x, y, w, h, text, col):
        d.add(Rect(x, y, w, h, rx=4, ry=4, fillColor=colors.HexColor('#F4F8F4'), strokeColor=col, strokeWidth=1))
        lines = text.split('\n')
        for i, t in enumerate(lines):
            d.add(String(x + w / 2, y + h - 11 - i * 9, t, fontName='Helvetica', fontSize=7.3, fillColor=C_TEXT,
                         textAnchor='middle'))

    def arrow(x1, y1, x2, y2):
        d.add(Line(x1, y1, x2, y2, strokeColor=C_EDGE, strokeWidth=1.1))
        import math
        a = math.atan2(y2 - y1, x2 - x1)
        for s in (2.6, -2.6):
            d.add(Line(x2, y2, x2 - 6 * math.cos(a) + s * math.sin(a), y2 - 6 * math.sin(a) - s * math.cos(a),
                       strokeColor=C_EDGE, strokeWidth=1.1))

    bw, bh = 100, 46
    box(4, 120, bw, bh, 'DS0/DS1 pins\n(GPIO17, GPIO3)\nread by the disk\ninterrupt', C_DS0)
    box(140, 120, bw + 20, bh, 'density differs from\nthe recorded zone\nof the track?\n-> request', C_DS1)
    box(300, 120, bw + 38, bh, 'main loop (core0):\nconvert track at that\ndensity (fast converter)\n+ resolve framing', C_EDGE)
    box(300, 30, bw + 38, bh, 'playback switches to the\nconverted stream at the\nsame disk angle\n(like a head step)', C_EDGE)
    box(140, 30, bw + 20, bh, 'cache: one converted\ntrack + density\n(drop on new image,\nstep or write)', C_GND)
    box(4, 30, bw, bh, 'same density as\nrecorded: normal\nplayback (no change)', C_GND)
    arrow(4 + bw, 143, 140, 143)
    arrow(140 + bw + 20, 143, 300, 143)
    arrow(369, 120, 369, 30 + bh)
    arrow(300, 53, 140 + bw + 20, 53)
    arrow(140, 53, 4 + bw, 53)
    return d


# --- density/zone diagram ----------------------------------------------------

def fig_cells():
    W, H = 165 * mm, 46 * mm
    d = Drawing(W, H)
    x0, x1 = 60, W - 10
    flux = [0, 2, 3, 5, 6, 8, 9, 11, 12, 13, 15, 17, 18, 19, 21, 23, 24, 26, 28]
    n = 28
    rec = (x1 - x0) / n
    y = H - 22
    d.add(String(4, y - 3, 'recorded', fontName='Helvetica-Bold', fontSize=7.5, fillColor=C_TEXT))
    d.add(String(4, y - 12, 'zone 2, 3.50 us', fontName='Helvetica', fontSize=7, fillColor=C_TEXT))
    for i in range(n + 1):
        d.add(Line(x0 + i * rec, y - 8, x0 + i * rec, y + 8, strokeColor=colors.HexColor('#BBBBBB'), strokeWidth=0.5))
    for f in flux:
        d.add(Line(x0 + f * rec, y - 9, x0 + f * rec, y + 9, strokeColor=C_DS1, strokeWidth=1.6))
    y2 = 26
    dec = rec * 3.25 / 3.5
    d.add(String(4, y2 - 3, 'drive reads', fontName='Helvetica-Bold', fontSize=7.5, fillColor=C_TEXT))
    d.add(String(4, y2 - 12, 'density 3, 3.25 us', fontName='Helvetica', fontSize=7, fillColor=C_TEXT))
    k = 0
    while x0 + k * dec <= x1:
        d.add(Line(x0 + k * dec, y2 - 8, x0 + k * dec, y2 + 8, strokeColor=C_DS0, strokeWidth=0.7))
        k += 1
    d.add(String(x0, 4, 'Same flux reversals (orange). The faster decoder clock (blue) fits more bitcells into long gaps '
                        '-> more bits per revolution.', fontName='Helvetica', fontSize=7, fillColor=C_TEXT))
    return d


# ---------------------------------------------------------------------------

def build():
    story = []
    story.append(P('1541-rePico: connecting the density select lines', H1))
    story.append(P('Instructions for option 1 (hardware modification, PCB v1.7) and option 2 (firmware that uses '
                   'the lines). Option 3, the per-image <b>.den</b> rules, is already in the firmware; see the last '
                   'page.', BODY))

    # --- background
    story.append(P('Why', H2))
    story.append(P('The 1541 has four read clocks ("densities"). The drive selects one with two VIA2 port B lines, '
                   'PB5 (DS0) and PB6 (DS1). DOS selects the density that matches the speed zone of the track. The '
                   'rePico board cannot see these lines: on PCB v1.7 they only pass through the VIA socket (IC1 pins '
                   '15/16, header P1 pins 15/16). The firmware therefore always plays a track at the rate it was '
                   'recorded with.', BODY))
    story.append(table([
        ['density (PB6 PB5)', 'DOS uses it on', 'bitcell', 'bytes per revolution'],
        ['3  (1 1)', 'tracks 1-17', '3.25 us', '7692'],
        ['2  (1 0)', 'tracks 18-24', '3.50 us', '7142'],
        ['1  (0 1)', 'tracks 25-30', '3.75 us', '6666'],
        ['0  (0 0)', 'tracks 31-35', '4.00 us', '6250']], [38 * mm, 38 * mm, 30 * mm, 40 * mm]))
    story.append(Spacer(1, 6))
    story.append(P('Some copy protections read a track with another density on purpose. The drive then decodes the '
                   'same flux reversals with another clock and sees more (or fewer) bits. Example: <b>RapidLok</b> '
                   '(Law of the West) reads its key track 36, recorded at zone 2 rate, with density 3. Between the two '
                   'syncs of that track it needs at least 256 bytes. Played at the recorded rate there are 253-254 '
                   '(the debug log on the board showed 254); read at density 3 a real drive and VICE see about 270.',
                   BODY))
    story.append(fig_cells())
    story.append(P('Figure 1: the same recorded track, decoded with the clock of another density.', CAP))
    story.append(P('Known titles: RapidLok (key track 36 at density 3; data tracks 32+ at density 3), A View To A Kill '
                   '(tracks 37-41), Rally Speedway (track 36).', SMALL))

    # --- option 1
    story.append(PageBreak())
    story.append(P('Option 1: hardware modification (PCB v1.7)', H1))
    story.append(P('All Pico GPIOs on the header are in use, so two pins are taken over:', BODY))
    story.append(table([
        ['line', 'VIA2 pin', 'goes to', 'what that pin did before'],
        ['DS0 (PB5)', 'IC1 pin 15', 'GPIO17 = Pico pin 22 = J8 pin 2', 'buzzer (BZ1 / J8). The buzzer is removed.'],
        ['DS1 (PB6)', 'IC1 pin 16', 'GPIO3 = Pico pin 5', 'SOE_GA (to the 1541-II gate array through Q9). The '
                                                          'firmware always drives it low; after the change the '
                                                          'board side is wired to GND instead.']],
        [24 * mm, 22 * mm, 50 * mm, 70 * mm]))
    story.append(Spacer(1, 4))
    story.append(fig_schematic())
    story.append(P('Figure 2: the modification as a circuit.', CAP))
    story.append(P('<b>Only for PCB v1.7.</b> On v1.6 GPIO17 is the SD card chip select. The firmware must be built '
                   'for the modified board (option 2); until then the lines are simply not used.', WARN))
    story.append(P('Parts and tools', H2))
    story.append(table([
        ['qty', 'part', 'note'],
        ['2', 'resistor 10 kOhm', 'in series with each VIA line: protects both sides, also when an old firmware '
                                  'drives the pin as an output'],
        ['2', 'resistor 22 kOhm', 'to GND: 5.0 V high level becomes 3.4 V at the Pico'],
        ['', 'thin insulated wire (wire-wrap, 0.25 mm)', 'about 15 cm'],
        ['', 'sharp knife, soldering iron, multimeter', 'desoldering braid or pump for the buzzer']],
        [12 * mm, 60 * mm, 94 * mm]))
    story.append(Spacer(1, 4))
    story.append(P('With 10k/22k a VIA high level of 5.0 V gives 3.44 V and 3.5 V gives 2.4 V at the Pico; both are '
                   'read as high (the RP2350 needs about 2.2 V). A low level stays below 0.3 V.', SMALL))

    story.append(P('Step 1: measure before changing anything', H2))
    story.append(P('Put the board into the drive as usual. Use DOS commands to move the head and measure DS0 (VIA '
                   'socket pin 15) and DS1 (pin 16) against GND (socket pin 1). DOS keeps the density after the '
                   'command. For each line in the table: <font face="Courier">OPEN15,8,15</font>, then '
                   '<font face="Courier">OPEN2,8,2,"#"</font> once, then the command.', BODY))
    story.append(table([
        ['C64 command', 'head on', 'DS1 pin 16', 'DS0 pin 15', 'density'],
        ['PRINT#15,"U1 2 0 1 0"', 'track 1', 'high', 'high', '3'],
        ['PRINT#15,"U1 2 0 18 0"', 'track 18', 'high', 'low', '2'],
        ['PRINT#15,"U1 2 0 25 0"', 'track 25', 'low', 'high', '1'],
        ['PRINT#15,"U1 2 0 31 0"', 'track 31', 'low', 'low', '0']],
        [50 * mm, 22 * mm, 24 * mm, 24 * mm, 18 * mm]))
    story.append(Spacer(1, 4))
    story.append(P('Write down the high level. 4.5-5.0 V is normal. If it is below 3.5 V, use 47 kOhm instead of '
                   '22 kOhm to GND (a larger resistor to GND divides less: 3.0 V then still gives 2.5 V at the Pico).', SMALL))

    story.append(KeepTogether([P('Step 2: orientation', H2),
                               Image(PHOTO_PCB, width=150 * mm, height=150 * mm * 1148 / 2040),
                               P('Figure 3: bare PCB v1.7, top (left) and bottom (right). R19 and Q9 are labelled '
                                 'on the bottom.', CAP)]))

    story.append(PageBreak())
    story.append(P('Step 3: free GPIO3 and connect DS1', H2))
    story.append(fig_bottom_full())
    story.append(P('Figure 4: bottom side, drawn from the KiCad file and mirrored as you see it when the board lies '
                   'upside down. The red trace is SOE_GA from Pico pin 5 to R19/Q9.', CAP))
    story.append(KeepTogether([fig_bottom_zoom(),
                               P('Figure 5: zoom. Dashed lines are the new wires.', CAP)]))
    story.append(P('1. Remove the Pico 2 from its socket strips and the board from the drive.', BODY))
    story.append(P('2. <b>Cut</b> the SOE_GA trace on the bottom side between the Pico pin 5 socket pad and R19 (red '
                   'mark): two cuts about 1 mm apart, remove the copper between them. The trace is 0.2 mm wide and '
                   'runs straight from pin 5 towards R19 (to the left in the bottom view, 3.7 mm long).', BODY))
    story.append(P('3. Check with the meter: Pico pin 5 pad to R19 pad 2 must be open now.', BODY))
    story.append(P('4. Solder a wire from <b>R19 pad 2</b> (the end next to Q9) to <b>GND</b>, for example the Pico '
                   'pin 3 socket pad. This keeps SOE_GA low, as the firmware did.', BODY))
    story.append(P('5. From <b>VIA socket pin 16</b>: 10 kOhm to <b>Pico pin 5</b> socket pad; from Pico pin 5: '
                   '22 kOhm to GND (Pico pin 3). Insulate the resistor leads (heat shrink) and keep them flat '
                   'against the board: the bottom side sits on the 1541 board.', BODY))

    story.append(PageBreak())
    story.append(P('Step 4: remove the buzzer and connect DS0', H2))
    story.append(Image(PHOTO_BUZ, width=110 * mm, height=110 * mm * 1148 / 2040))
    story.append(P('Figure 6: assembled board, the buzzer BZ1 is at the corner next to the VIA socket.', CAP))
    story.append(fig_top_buzzer())
    story.append(P('Figure 7: top side at BZ1/J8. The new wires are on the bottom side (figure 4).', CAP))
    story.append(P('1. Desolder the buzzer BZ1 (two pins), or unplug the sound module from J8 if you use one.', BODY))
    story.append(P('2. On the bottom side (figure 4): from <b>VIA socket pin 15</b> (DS0) a wire through the middle of the VIA '
                   'socket to <b>J8 pin 2</b> (GPIO17), with 10 kOhm in it.', BODY))
    story.append(P('3. 22 kOhm from <b>J8 pin 2</b> to <b>J8 pin 3</b> (GND), on the bottom side. J8 pin 1 is 3.3 V: do not use it.', BODY))

    story.append(P('Step 5: check', H2))
    story.append(P('1. Without the Pico: no short between DS0, DS1, GND, 3.3 V and 5 V. R19 pad 2 to GND: 0 Ohm. '
                   'Pico pin 5 pad to GND: about 22 kOhm. J8.2 to GND: about 22 kOhm.', BODY))
    story.append(P('2. Insert the Pico, put the board into the drive, power on. Repeat the table of step 1 and measure '
                   'at Pico pin 5 (DS1) and J8 pin 2 (DS0): high about 3.4 V, low below 0.3 V.', BODY))
    story.append(P('3. With the old firmware the beeper setting has no effect any more; the drive must work as before '
                   '(the board pins now sit behind 10 kOhm). Normal loading must work: SOE_GA is still low.', BODY))
    story.append(P('For a next PCB revision', H3))
    story.append(P('Route DS0/DS1 through two more BSS138 level shifter channels (like the other VIA signals) to the '
                   'GPIOs used here, tie SOE_GA low on the board, and drop the buzzer or move it to an I2C port '
                   'expander on the display bus.', BODY))

    # --- option 2
    story.append(PageBreak())
    story.append(P('Option 2: firmware for the modified board', H1))
    story.append(P('Once the lines are connected, the firmware can see the density the drive selects and convert a '
                   'track when it differs from the zone the track was recorded at. The converter from option 3 '
                   '(<font face="Courier">gcr_resample()</font> in <font face="Courier">src/gcr_resolve.c</font>) '
                   'already does the conversion; option 2 runs it at runtime instead of at load time.', BODY))
    story.append(fig_flow())
    story.append(P('Figure 8: data flow with the density lines.', CAP))
    story.append(P('Changes', H2))
    story.append(table([
        ['file', 'change'],
        ['include/pinout.h, CMakeLists.txt', 'new build option <font face="Courier">DENSITY_SENSE</font> (PCB v1.7 '
                                             'only): GPIO_DS0 = 17, GPIO_DS1 = 3; GPIO_SND and GPIO_SOE_GA not used'],
        ['src/main.c, src/drive.c', 'inputs without pulls; no buzzer and no SOE_GA output in that build; hide the '
                                    'Beeper setting'],
        ['src/drive.c', 'read DS0/DS1 at every playback event (one GPIO read); when the density differs from '
                        'd64_track_zone of the track under the head, request a conversion like a head step'],
        ['src/gcr_resolve.c', 'faster converter: one step per flux interval instead of per decoder clock (closed '
                              'form: a 1, then zeros with the UF4 wrap, random bits after about 18 us)'],
        ['src/drive.c (service_track_resolver)', 'convert into a scratch track (8 KB), resolve, switch playback at '
                                                 'the same angle; cache key = track + density'],
        ['src/rw_routines.c', 'writes go to the recorded track; a converted copy is dropped after a write'],
        ['debug log', 'line "DENS track N: density d (recorded zone z)" on every change']],
        [52 * mm, 114 * mm]))
    story.append(P('Timing', H2))
    story.append(P('RapidLok writes the density right after the head has settled (about 6 ms after the last step) '
                   'and starts reading about 2.5 ms later. The current converter needs 0.3-0.4 ms per track on the '
                   'PC, which is roughly 70-100 ms on the Pico (track analysis: 0.015 ms on the PC, 2-5 ms on the '
                   'board). That is fine at load time (option 3) but too slow here. With one step per flux interval '
                   '(about 20 000 per track) the conversion should take about 1-2 ms, so convert + resolve fits into '
                   'the 2.5 ms in most cases. If not, the first read fails and RapidLok retries (it tries the key '
                   'check up to 60 times).', BODY))
    story.append(P('Memory', H2))
    story.append(P('The normal build uses 493 696 of 524 288 bytes RAM (about 30 KB free). One converted track (8 KB) '
                   'fits; the existing double-buffered framing is reused. Alternatively the free tail of the half-track '
                   'pool can hold it (option 3 does this at load time).', BODY))
    story.append(KeepTogether([P('Tests', H2), table([
        ['test', 'how'],
        ['converter vs VICE', '<font face="Courier">gcr_resolve_test --density-check image.g64 32 36 3</font>: '
                              'bytes after every sync compared with the VICE read circuit at that density'],
        ['RapidLok key on the PC', 'drive_harness scenario 7: track 36 has 256 or more bytes between the syncs'],
        ['runtime switch', 'new harness scenario: change DS0/DS1 while playing, check the angle and the bytes '
                           'after the switch'],
        ['board', 'Law of the West (no .den file), A View To A Kill, Rally Speedway, plus normal D64/G64 '
                  'loading, SAVE/FORMAT and the selector'],
        ['debug log', 'DENS lines match the table of option 1 step 1 when DOS moves between zones']],
        [42 * mm, 124 * mm])]))

    # --- option 3
    story.append(PageBreak())
    story.append(P('Option 3 (available now): density rules per image', H1))
    story.append(P('Without the hardware change, a text file next to a G64 image tells the firmware which tracks a '
                   'loader reads at another density. It has the same name as the image and the extension '
                   '<b>.den</b>. When the image is opened, these tracks are converted once into the bit stream the '
                   'drive decodes at that density (about 0.1 s per track).', BODY))
    story.append(table([
        ['file', 'content'],
        ['Law_of_the_West_(Accolade)_(1985)_(original)_-PAL-_k2_s0.den',
         '<font face="Courier"># Law of the West (Accolade 1985), RapidLok 1<br/>32-36 3</font>']],
        [80 * mm, 86 * mm]))
    story.append(Spacer(1, 4))
    story.append(P('Format: one rule per line, <font face="Courier">first[-last] density</font>, tracks 1-42, density '
                   '0-3 (3 = fastest, as on tracks 1-17). Lines starting with # are comments. The file browser does '
                   'not list .den files. Half-tracks are not converted. "Save Image (G64)" stores the converted '
                   'tracks (marked as speed 3); such a saved image needs no .den file on the board, but it is not '
                   'the original image any more.', BODY))
    story.append(P('The rule depends on the loader, not on the game: derive it from the loader code or from VICE '
                   '(RapidLok 1 in Law of the West: tracks 1-17 and 32+ at density 3, 19-31 at density 2; only 32+ '
                   'differ from the recorded zone).', SMALL))

    doc = SimpleDocTemplate(OUT, pagesize=A4, leftMargin=20 * mm, rightMargin=20 * mm, topMargin=16 * mm,
                            bottomMargin=16 * mm, title='1541-rePico: density select lines',
                            author='1541-rePico')

    def footer(canvas, doc_):
        canvas.saveState()
        canvas.setFont('Helvetica', 7.5)
        canvas.setFillColor(colors.HexColor('#666666'))
        canvas.drawString(20 * mm, 9 * mm, '1541-rePico - density select lines (DS0/DS1)')
        canvas.drawRightString(A4[0] - 20 * mm, 9 * mm, 'page %d' % doc_.page)
        canvas.restoreState()

    doc.build(story, onFirstPage=footer, onLaterPages=footer)
    print('wrote', OUT)


if __name__ == '__main__':
    build()
