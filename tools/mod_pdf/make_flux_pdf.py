"""Build doc/flux-mod-1541-II.pdf: step-by-step instructions with large pictures for the
1541-FluxPico flux mod (PCB v1.7 in a 1541-II with the 40-pin 251828 gate array).

Pad positions come from the KiCad PCB file (pcb.py / figs.py); the gate array pins from the
251828 (64H156) 40-pin pinout; the photos of the drive board and of the user's rePico v1.7 are
from doc/pictures.
"""
import os

from PIL import Image as PImage, ImageDraw, ImageFont
from reportlab.graphics.shapes import Circle, Drawing, Line, Polygon, Rect, String
from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.platypus import (Image, KeepTogether, PageBreak, Paragraph, SimpleDocTemplate, Spacer, Table,
                                TableStyle)

from figs import B

REPO = r'E:\GitHub\1541-rePico'
OUT = os.environ.get('PDF_OUT', os.path.join(REPO, 'doc', 'flux-mod-1541-II.pdf'))
TMP = os.environ.get('PDF_TMP', os.path.join(REPO, 'tools', 'mod_pdf', '_tmp'))
PHOTO_BOARD = os.path.join(REPO, 'doc', 'pictures', 'flux_mod_board.jpg')

# strong, distinct colours (also readable when printed in grey)
C_TEXT = colors.HexColor('#111111')
C_EDGE = colors.HexColor('#1E5A1E')
C_A = colors.HexColor('#0B5FD8')      # wire A: MODE
C_B = colors.HexColor('#D97000')      # wire B: SOE
C_C = colors.HexColor('#C0007A')      # wire C: flux
C_CUT = colors.HexColor('#D10000')
C_GND = colors.HexColor('#333333')
C_5V = colors.HexColor('#A05A00')
C_CHK = colors.HexColor('#2E7D32')

styles = getSampleStyleSheet()
H1 = ParagraphStyle('H1', parent=styles['Heading1'], fontSize=20, leading=24, spaceAfter=8, textColor=C_EDGE)
H2 = ParagraphStyle('H2', parent=styles['Heading2'], fontSize=15, leading=19, spaceBefore=8, spaceAfter=6,
                    textColor=C_EDGE)
BODY = ParagraphStyle('B', parent=styles['BodyText'], fontSize=12, leading=16, spaceAfter=6)
STEP = ParagraphStyle('S', parent=BODY, leftIndent=18, firstLineIndent=-18, spaceAfter=7)
CAP = ParagraphStyle('C', parent=BODY, fontSize=10.5, leading=13, alignment=TA_CENTER, spaceBefore=2,
                     spaceAfter=10, textColor=colors.HexColor('#333333'))
NOTE = ParagraphStyle('N', parent=BODY, backColor=colors.HexColor('#FFF4E0'), borderPadding=7,
                      borderColor=colors.HexColor('#E0B060'), borderWidth=1, spaceBefore=6, spaceAfter=12)
WARN = ParagraphStyle('W', parent=NOTE, backColor=colors.HexColor('#FDE6E6'), borderColor=C_CUT)


def P(text, st=BODY):
    return Paragraph(text, st)


def table(rows, widths, size=11.5):
    data = [[Paragraph(str(c), ParagraphStyle('t', parent=BODY, fontSize=size, leading=size + 3, spaceAfter=0))
             for c in r] for r in rows]
    t = Table(data, colWidths=widths, hAlign='LEFT')
    t.setStyle(TableStyle([('GRID', (0, 0), (-1, -1), 0.8, colors.HexColor('#777777')),
                           ('VALIGN', (0, 0), (-1, -1), 'MIDDLE'),
                           ('BACKGROUND', (0, 0), (-1, 0), colors.HexColor('#DDEBDD')),
                           ('TOPPADDING', (0, 0), (-1, -1), 4), ('BOTTOMPADDING', (0, 0), (-1, -1), 5)]))
    return t


def text(d, x, y, s, size=11, col=C_TEXT, anchor='start', bold=True):
    d.add(String(x, y, s, fontName='Helvetica-Bold' if bold else 'Helvetica', fontSize=size, fillColor=col,
                 textAnchor=anchor))


def tag(d, x, y, s, col, size=11, anchor='start'):
    """text in a white box with a coloured frame"""
    w = len(s) * size * 0.58 + 8
    left = x if anchor == 'start' else (x - w if anchor == 'end' else x - w / 2)
    d.add(Rect(left, y - size * 0.45, w, size * 1.55, fillColor=colors.white, strokeColor=col, strokeWidth=1.6))
    text(d, left + 4, y, s, size, col)


# --- gate array (251828, 40 pins) ---------------------------------------------

GA_PINS = {1: 'TEST', 2: 'YB0', 3: 'YB1', 4: 'YB2', 5: 'YB3', 6: 'YB4', 7: 'YB5', 8: 'YB6', 9: 'YB7', 10: 'VSS (GND)',
           11: 'STP1', 12: 'STP0', 13: 'MTR', 14: 'A', 15: 'DS0', 16: 'DS1', 17: 'SYNC', 18: 'TED', 19: 'OE (MODE)',
           20: 'ACCL (GND)', 21: 'OSC', 22: 'ATNA', 23: 'ATNI', 24: 'ATN', 25: 'Y0', 26: 'Y1', 27: 'Y2', 28: 'Y3',
           29: 'XRW', 30: 'VCC (+5V)', 31: 'CLR', 32: 'PLL (GND)', 33: 'LOCK (GND)', 34: 'R/W', 35: 'Q', 36: 'XQ',
           37: 'CK', 38: 'B', 39: 'SOE', 40: 'BYTE'}


def pin_xy(n, x0, ytop, step, bw):
    """pin end point of a DIP-40 seen from above, notch at the top; left row 1..20 down, right row 21..40 up"""
    if n <= 20:
        return x0, ytop - (n - 0.5) * step, -1
    return x0 + bw, ytop - (40 - n + 0.5) * step, 1


def fig_gate_array(marks, title, step=15.5):
    """marks: {pin: (colour, label)}"""
    W = 165 * mm
    rows = 20
    H = rows * step + 46
    d = Drawing(W, H)
    bw = 92
    x0 = W / 2 - bw / 2
    ytop = H - 30
    # body + notch
    d.add(Rect(x0, ytop - rows * step, bw, rows * step, fillColor=colors.HexColor('#2A2A2A'), strokeColor=C_TEXT,
               strokeWidth=1.2))
    d.add(Circle(x0 + bw / 2, ytop, 9, fillColor=colors.white, strokeColor=C_TEXT, strokeWidth=1))
    d.add(Rect(x0 + bw / 2 - 10, ytop, 20, 10, fillColor=colors.white, strokeColor=None))
    text(d, x0 + bw / 2, ytop + 8, 'notch', 10, C_TEXT, 'middle', bold=False)
    text(d, x0 + bw / 2, ytop - rows * step / 2 + 10, '251828', 13, colors.white, 'middle')
    text(d, x0 + bw / 2, ytop - rows * step / 2 - 6, 'gate array', 10, colors.white, 'middle', bold=False)
    d.add(Circle(x0 + 12, ytop - 12, 3.5, fillColor=colors.HexColor('#888888'), strokeColor=None))
    for n in range(1, 41):
        x, y, side = pin_xy(n, x0, ytop, step, bw)
        col = marks.get(n, (None, None))[0]
        pw = 14
        px = x - pw if side < 0 else x
        d.add(Rect(px, y - 4, pw, 8, fillColor=col or colors.HexColor('#C8C8C8'),
                   strokeColor=C_TEXT, strokeWidth=0.6))
        nx = x + 4 if side < 0 else x - 4
        text(d, nx, y - 3.6, str(n), 10, colors.white, 'start' if side < 0 else 'end')
        if n in marks:
            lab = '%d  %s' % (n, marks[n][1])
            ex = x - pw - 12 if side < 0 else x + pw + 12
            d.add(Line(x - pw * (side < 0) + pw * (side > 0), y, ex, y, strokeColor=col, strokeWidth=1.6))
            tag(d, ex, y - 3.5, lab, col, 11, 'end' if side < 0 else 'start')
    text(d, W / 2, H - 12, title, 12.5, C_TEXT, 'middle')
    return d


def fig_clip_leg():
    """side view: cut leg 31 near the board, bend the top part away, wire to it"""
    W, H = 165 * mm, 150
    d = Drawing(W, H)
    for i, (x0, cap) in enumerate(((40, '1. cut here, close to the board'), (260, '2. bend out, solder wire C'))):
        d.add(Rect(x0 - 20, 20, 170, 10, fillColor=colors.HexColor('#2E7D32'), strokeColor=C_TEXT))  # PCB
        text(d, x0 - 16, 6, 'drive main board', 10, C_TEXT, bold=False)
        d.add(Rect(x0, 70, 120, 34, fillColor=colors.HexColor('#2A2A2A'), strokeColor=C_TEXT))      # chip body
        text(d, x0 + 60, 84, 'gate array', 10.5, colors.white, 'middle')
        # neighbour legs (grey) and leg 31
        for k, lx in enumerate((x0 + 30, x0 + 60, x0 + 90)):
            mine = (k == 1)
            col = C_CUT if mine else colors.HexColor('#A0A0A0')
            if i == 0 or not mine:
                d.add(Rect(lx - 3, 30, 6, 40, fillColor=col, strokeColor=C_TEXT, strokeWidth=0.5))
            else:
                d.add(Rect(lx - 3, 30, 6, 10, fillColor=col, strokeColor=C_TEXT, strokeWidth=0.5))   # stub
                d.add(Polygon([lx - 3, 70, lx + 3, 70, lx + 15, 46, lx + 9, 46], fillColor=col, strokeColor=C_TEXT,
                              strokeWidth=0.5))
                d.add(Line(lx + 12, 46, lx + 80, 56, strokeColor=C_C, strokeWidth=2.2))
                tag(d, lx + 82, 52, 'wire C', C_C, 11)
            text(d, lx, 112, ('30', '31', '32')[k], 11, col if mine else C_TEXT, 'middle')
        if i == 0:
            for dx in (-1.2, 1.2):
                d.add(Line(x0 + 50, 40 + dx * 2, x0 + 70, 40 + dx * 2, strokeColor=C_CUT, strokeWidth=2))
        text(d, x0 + 60, 134, cap, 11, C_TEXT, 'middle')
    return d


# --- rePico board ---------------------------------------------------------------

# Photos of the user's own rePico v1.7 (doc/pictures, 2400 x 3200). All positions below are in
# "photo units": the photo scaled to 900 x 1200. Each photo is matched to the KiCad board by four
# VIA socket pads (a perspective transform), so every mark sits on the pad from the PCB file.
PHOTO_BOTTOM = os.path.join(REPO, 'doc', 'pictures', 'repico_v17_bottom.jpg')
PHOTO_TOP = os.path.join(REPO, 'doc', 'pictures', 'repico_v17_top.jpg')
CAL_BOTTOM = [(('IC1', 20), (440, 212)), (('IC1', 1), (440, 855)), (('IC1', 21), (640, 212)),
              (('IC1', 40), (640, 855))]
CAL_TOP = [(('IC1', 20), (521, 386)), (('IC1', 1), (522, 836)), (('IC1', 21), (371, 384)),
           (('IC1', 40), (374, 833))]
# the plug pins stand ~10 mm off the board and lean in the photo (perspective): their marks go on
# the pin tips, matched by the tips of the four end pins
CAL_BOTTOM_TIPS = [(('P1', 20), (398, 183)), (('P1', 1), (388, 916)), (('P2', 1), (627, 183)),
                   (('P2', 20), (615, 913))]
PLUG_PINS = ('P1', 'P2')

P_RED, P_BLUE, P_ORANGE = (215, 0, 0), (0, 85, 225), (225, 110, 0)
P_PURPLE, P_GREY, P_GREEN = (175, 0, 130), (70, 70, 70), (20, 120, 30)


def _homography(cal):
    import numpy as np
    rows = []
    for (ref, num), (u, v) in cal:
        p = B.pad(ref, num)
        x, y = p['x'], p['y']
        rows.append([x, y, 1, 0, 0, 0, -u * x, -u * y, -u])
        rows.append([0, 0, 0, x, y, 1, -v * x, -v * y, -v])
    h = np.linalg.svd(np.array(rows))[2][-1]
    h = h.reshape(3, 3) / h[-1]

    def to_photo(ref, num):
        p = B.pad(ref, num)
        q = h @ [p['x'], p['y'], 1]
        return q[0] / q[2], q[1] / q[2]
    return to_photo


def photo_figure(src, cal, crop, marks, boxes, text_h, name, tips=None):
    """marks: (ref, pin, colour, text, label_x, label_y): ring on the pad (on the tip for a plug pin
    when tips is given), label with a leader line.
    boxes: (refs, colour, text, label_x, label_y): frame around the pads of parts, label next to it.
    crop and label positions in photo units; text_h: letter height in photo units."""
    os.makedirs(TMP, exist_ok=True)
    im = PImage.open(src).convert('RGB')
    s = im.width / 900.0
    dr = ImageDraw.Draw(im)
    f = ImageFont.truetype('C:/Windows/Fonts/arialbd.ttf', int(text_h * s))
    lw = max(3, int(text_h * s / 9))
    at = _homography(cal)
    at_tip = _homography(tips) if tips else None

    def label(x, y, txt, col, to):
        x, y = x * s, y * s
        tw = dr.textlength(txt, font=f)
        pad = text_h * s * 0.25
        bx = (x - pad, y - pad, x + tw + pad, y + text_h * s * 1.15 + pad)
        # leader from the nearest box edge to the target
        tx, ty = to[0] * s, to[1] * s
        cx, cy = min(max(tx, bx[0]), bx[2]), min(max(ty, bx[1]), bx[3])
        dr.line((cx, cy, tx, ty), fill=col, width=lw)
        dr.rectangle(bx, fill='white', outline=col, width=lw)
        dr.text((x, y), txt, fill=col, font=f)

    for ref, num, col, txt, lx, ly in marks:
        u, v = (at_tip if at_tip and ref in PLUG_PINS else at)(ref, num)
        r = 11 * s
        if at_tip and ref in PLUG_PINS:
            # the visible pin: a line from the top of the black plastic (about halfway between the
            # board and the tip in the photo) to the tip
            bu, bv = at(ref, num)
            bu, bv = bu + 0.45 * (u - bu), bv + 0.45 * (v - bv)
            dr.line((bu * s, bv * s, u * s, v * s), fill=col, width=int(lw * 2.2))
            r = 7 * s
        dr.ellipse((u * s - r, v * s - r, u * s + r, v * s + r), outline=col, width=int(lw * 1.6))
        if txt:
            label(lx, ly, txt, col, (u + (lx > u and 11 or -11) * 0.7, v))
    for parts, col, txt, lx, ly in boxes:
        pts = [at(ref, p['num']) for ref in parts for p in B.footprints[ref]['pads']]
        x0, y0 = min(p[0] for p in pts) - 10, min(p[1] for p in pts) - 9
        x1, y1 = max(p[0] for p in pts) + 10, max(p[1] for p in pts) + 9
        dr.rectangle((x0 * s, y0 * s, x1 * s, y1 * s), outline=col, width=int(lw * 1.6))
        label(lx, ly, txt, col, (x1 if lx > x1 else x0, (y0 + y1) / 2))
    out = os.path.join(TMP, name)
    c = im.crop(tuple(int(v * s) for v in crop))
    c.save(out, quality=88)
    return out, c.size


def photo_top():
    marks = [('U1', 1, P_GREY, 'Pico pin 1', 640, 300),
             ('U1', 5, P_PURPLE, 'Pico pin 5 (wire C)', 640, 520),
             ('IC1', 1, P_GREY, 'VIA pin 1', 560, 905)]
    boxes = []
    return photo_figure(PHOTO_TOP, CAL_TOP, (230, 280, 900, 960), marks, boxes, 20, 'repico_top.jpg')


def photo_bottom():
    marks = [('P1', 19, P_RED, 'CLIP', 230, 112),
             ('IC1', 19, P_BLUE, 'wire A', 480, 112),
             ('U1', 5, P_PURPLE, 'wire C', 120, 400),
             ('P2', 19, P_RED, 'CLIP', 450, 985),
             ('IC1', 39, P_ORANGE, 'wire B', 690, 770)]
    boxes = [(('Q1',), P_RED, 'Q1: REMOVE', 600, 420)]
    return photo_figure(PHOTO_BOTTOM, CAL_BOTTOM, (30, 95, 860, 1020), marks, boxes, 19, 'repico_bottom.jpg',
                        CAL_BOTTOM_TIPS)


def photo_zoom_q1():
    marks = [('P1', 20, P_GREY, 'top plug pin', 290, 150),
             ('P1', 19, P_RED, 'CLIP', 300, 250),
             ('IC1', 20, P_GREY, '+5V (VIA 20)', 470, 150),
             ('IC1', 19, P_BLUE, 'wire A', 395, 395),
             ('U1', 1, P_GREY, 'Pico pin 1', 210, 205),
             ('U1', 5, P_PURPLE, 'wire C (Pico pin 5)', 205, 300),
             ('Q1', 2, P_GREEN, 'leg 2 = GND', 470, 395),
             ('Q1', 3, P_RED, 'leg 3', 545, 345)]
    boxes = [(('Q1',), P_RED, 'Q1: REMOVE', 548, 310),
             (('Q6',), P_GREY, 'Q6: do not touch', 548, 263),
             (('Q8',), P_GREY, 'Q8: do not touch', 548, 216)]
    return photo_figure(PHOTO_BOTTOM, CAL_BOTTOM, (195, 140, 660, 420), marks, boxes, 10, 'repico_zoom_q1.jpg',
                        CAL_BOTTOM_TIPS)


def photo_zoom_p2():
    marks = [('P2', 20, P_GREY, 'bottom plug pin', 450, 900),
             ('P2', 19, P_RED, 'CLIP', 500, 745),
             ('IC1', 39, P_ORANGE, 'wire B', 680, 745),
             ('IC1', 1, P_GREY, 'GND (VIA 1)', 380, 960)]
    return photo_figure(PHOTO_BOTTOM, CAL_BOTTOM, (360, 720, 780, 1000), marks, [], 10, 'repico_zoom_p2.jpg',
                        CAL_BOTTOM_TIPS)


def fig_header_clip():
    """side view of the plug pins: cut pin 19 flush under the black plastic"""
    W, H = 165 * mm, 150
    d = Drawing(W, H)
    d.add(Rect(40, 110, 380, 10, fillColor=colors.HexColor('#2E7D32'), strokeColor=C_TEXT))
    text(d, 44, 126, 'rePico board (upside down in your hand: this side is the bottom)', 10.5, C_TEXT, bold=False)
    d.add(Rect(52, 96, 312, 14, fillColor=colors.HexColor('#202020'), strokeColor=C_TEXT))
    text(d, 220, 100, 'black plastic of the pin header', 9.5, colors.white, 'middle', bold=False)
    for k in range(9):
        x = 70 + k * 36
        pin19 = (k == 7)
        col = C_CUT if pin19 else colors.HexColor('#C9A400')
        if pin19:
            d.add(Rect(x - 3, 92, 6, 4, fillColor=col, strokeColor=C_TEXT, strokeWidth=0.4))
            d.add(Rect(x - 3, 20, 6, 60, fillColor=None, strokeColor=col, strokeWidth=1, strokeDashArray=[3, 2]))
            for dy in (-1.3, 1.3):
                d.add(Line(x - 14, 90 + dy * 2, x + 14, 90 + dy * 2, strokeColor=C_CUT, strokeWidth=2))
            d.add(Line(x + 14, 90, x + 40, 66, strokeColor=C_CUT, strokeWidth=1.2))
            tag(d, x + 40, 60, 'cut flush', C_CUT, 11)
            text(d, x, 8, 'CLIP', 11, C_CUT, 'middle')
        else:
            d.add(Rect(x - 3, 20, 6, 76, fillColor=col, strokeColor=C_TEXT, strokeWidth=0.4))
    text(d, 70 + 8 * 36 + 14, 8, '<- end of the row', 10, C_TEXT, 'start', bold=False)
    return d


def fig_wiring():
    W, H = 165 * mm, 200
    d = Drawing(W, H)
    lx, lw = 6, 150
    gx, gw = W - 160, 154
    d.add(Rect(lx, 20, lw, 160, fillColor=colors.HexColor('#E8F2E8'), strokeColor=C_EDGE, strokeWidth=1.5))
    text(d, lx + lw / 2, 164, 'rePico (bottom)', 13, C_EDGE, 'middle')
    d.add(Rect(gx, 20, gw, 160, fillColor=colors.HexColor('#EEEEEE'), strokeColor=C_TEXT, strokeWidth=1.5))
    text(d, gx + gw / 2, 164, 'gate array 251828', 13, C_TEXT, 'middle')
    for y, col, a, b, name in ((130, C_A, 'mark "wire A"', 'pin 19 (OE)', 'A'),
                               (90, C_B, 'mark "wire B"', 'pin 39 (SOE)', 'B'),
                               (50, C_C, 'mark "wire C"', 'pin 31 (CLR)', 'C')):
        text(d, lx + 10, y - 4, a, 12.5, col)
        text(d, gx + 10, y - 4, b, 12.5, col)
        d.add(Line(lx + lw, y, gx, y, strokeColor=col, strokeWidth=3))
        tag(d, (lx + lw + gx) / 2, y + 5, 'wire ' + name, col, 12, 'middle')
    text(d, W / 2, 4, 'three wires, about 10-15 cm each', 11, C_TEXT, 'middle', bold=False)
    return d


def fig_before_after():
    W, H = 165 * mm, 175
    d = Drawing(W, H)

    def box(x, y, w, h, title, lines, col):
        d.add(Rect(x, y, w, h, rx=6, ry=6, fillColor=colors.white, strokeColor=col, strokeWidth=1.8))
        text(d, x + w / 2, y + h - 16, title, 12.5, col, 'middle')
        for i, s in enumerate(lines):
            text(d, x + w / 2, y + h - 34 - i * 14, s, 10.5, C_TEXT, 'middle', bold=False)

    half = W / 2 - 8
    box(4, 8, half, H - 16, 'BEFORE (rePico)', ['the Pico computes bytes', 'and puts them on port A,',
                                                 'pulses BYTE READY and SYNC.', '', 'the gate array is held off',
                                                 '(MODE pin tied to GND)', '', 'density: guessed (.den files)'],
        colors.HexColor('#777777'))
    box(W / 2 + 4, 8, half, H - 16, 'AFTER (FluxPico)', ['the Pico sends flux pulses', 'into the gate array (CLR),',
                                                          'like the read head would.', '',
                                                          'the gate array decodes, makes', 'SYNC, BYTE READY, port A',
                                                          '', 'density: the drive\'s own'], C_EDGE)
    return d


# --- photo with notes -------------------------------------------------------------

# pin centres in the user's photo (768 x 1024, board upside down: all print reads upside down)
GA_TOP_Y, GA_BOT_Y, GA_X_LEFT, GA_X_RIGHT = 422, 503, 357, 613   # gate array U10 leg rows
SO_TOP_Y, SO_BOT_Y, SO_X_LEFT, SO_X_RIGHT = 547, 629, 354, 620   # VIA socket U8 holes


def photo_pin(n, top_y, bot_y, xl, xr):
    """board upside down: pin 1 is top right, pins 1-20 run right to left along the top row,
    pins 21-40 left to right along the bottom row"""
    step = (xr - xl) / 19.0
    if n <= 20:
        return xr - (n - 1) * step, top_y
    return xl + (n - 21) * step, bot_y


def annotated_photos():
    """overview of the user's board and a zoom of the gate array with the pins marked"""
    os.makedirs(TMP, exist_ok=True)
    src = PImage.open(PHOTO_BOARD).convert('RGB')
    k = 3                                               # work at 3x for clean marks
    big = src.resize((src.width * k, src.height * k), PImage.LANCZOS)
    dr = ImageDraw.Draw(big)
    f = ImageFont.truetype('C:/Windows/Fonts/arialbd.ttf', 64)

    def label(x, y, txt, col):
        tw = dr.textlength(txt, font=f)
        dr.rectangle((x - 10, y - 6, x + tw + 10, y + 74), fill='white', outline=col, width=6)
        dr.text((x, y), txt, fill=col, font=f)

    # overview
    ov = big.copy()
    do = ImageDraw.Draw(ov)
    fo = ImageFont.truetype('C:/Windows/Fonts/arialbd.ttf', 70)

    def olabel(x, y, txt, col):
        tw = do.textlength(txt, font=fo)
        do.rectangle((x - 12, y - 8, x + tw + 12, y + 82), fill='white', outline=col, width=7)
        do.text((x, y), txt, fill=col, font=fo)
    do.rectangle((335 * k, 405 * k, 640 * k, 520 * k), outline=(200, 0, 0), width=14)
    olabel(60 * k, 360 * k, 'gate array 251828 (U10)', (200, 0, 0))
    do.rectangle((335 * k, 525 * k, 640 * k, 650 * k), outline=(0, 90, 220), width=14)
    olabel(90 * k, 655 * k, 'VIA socket U8 (rePico)', (0, 90, 220))
    do.rectangle((485 * k, 280 * k, 610 * k, 385 * k), outline=(110, 110, 110), width=10)
    olabel(470 * k, 230 * k, 'U12', (110, 110, 110))
    out1 = os.path.join(TMP, 'board_overview.jpg')
    ov.resize((src.width * 2, src.height * 2), PImage.LANCZOS).save(out1, quality=90)

    # zoom: gate array + VIA socket with pin numbers
    red, blue, orange, grey = (210, 0, 0), (0, 90, 220), (220, 110, 0), (70, 70, 70)

    def ring(n, rows, col, r=8, w=5):
        x, y = photo_pin(n, *rows)
        dr.ellipse(((x - r) * k, (y - r) * k, (x + r) * k, (y + r) * k), outline=col, width=w)
        return x, y

    def centred(x, y, txt, col):
        label(x * k - dr.textlength(txt, font=f) / 2, y * k, txt, col)

    ga = (GA_TOP_Y, GA_BOT_Y, GA_X_LEFT, GA_X_RIGHT)
    so = (SO_TOP_Y, SO_BOT_Y, SO_X_LEFT, SO_X_RIGHT)
    # gate array: top row labels above the chip, bottom row labels in the gap above the socket
    x, y = ring(1, ga, grey)
    centred(x - 6, y - 38, '1', grey)
    x, y = ring(19, ga, blue)
    centred(x + 10, y - 38, '19 = A', blue)
    ring(30, ga, grey, 7, 4)
    ring(32, ga, grey, 7, 4)
    x, y = ring(31, ga, red, 10, 8)
    centred(x, y + 13, '31 CUT', red)
    x, y = ring(39, ga, orange)
    centred(x - 4, y + 13, '39 = B', orange)
    # bracket under the bottom row: legs 21..31 counted from the left
    x21, yb = photo_pin(21, *ga)
    dr.line((x21 * k, (yb - 13) * k, (x - 0) * k, (yb - 13) * k), fill=red, width=5)
    for xx in [photo_pin(n, *ga)[0] for n in range(21, 32)]:
        dr.line((xx * k, (yb - 16) * k, xx * k, (yb - 10) * k), fill=red, width=4)
    # VIA socket: labels inside the empty socket window
    x, y = ring(1, so, blue)
    centred(x - 10, y + 12, 'VIA 1', blue)
    x, y = ring(19, so, blue)
    centred(x + 12, y + 12, 'VIA 19', blue)
    x, y = ring(39, so, orange)
    centred(x - 14, y - 36, 'VIA 39', orange)
    zoom = big.crop((325 * k, 360 * k, 660 * k, 690 * k))
    out2 = os.path.join(TMP, 'board_zoom.jpg')
    zoom.save(out2, quality=90)
    return (out1, ov.size), (out2, zoom.size)


# --- document -------------------------------------------------------------------------

def build():
    s = []
    s.append(P('1541-FluxPico: flux mod for the 1541-II', H1))
    s.append(P('For the rePico board <b>v1.7</b> in a <b>1541-II</b> (main board ASSY 340503) with the 40-pin '
               'gate array <b>251828</b>. After this mod the Pico sends flux pulses into the drive\'s own read '
               'circuit, and the drive decodes them itself.', BODY))
    s.append(fig_before_after())
    s.append(P('What you change', H2))
    s.append(table([
        ['where', 'what', 'step'],
        ['drive main board', 'cut leg 31 of the gate array, solder wire C to it', '2'],
        ['rePico, bottom', 'clip two plug pins (red CLIP in the photos)', '3'],
        ['rePico, bottom', 'remove the small transistor Q1', '4'],
        ['rePico -> gate array', 'three wires: A (MODE), B (SOE), C (flux)', '5']],
        [44 * mm, 100 * mm, 20 * mm]))
    s.append(Spacer(1, 8))
    s.append(P('<b>The buzzer stays.</b> No level shifter module and no extra parts are needed.', BODY))
    s.append(P('<b>After the mod, never run the old rePico firmware.</b> It drives port A, SYNC and BYTE READY '
               'itself, but after the mod the gate array drives those lines: both would push against each other. '
               'It would also send its old SOE signal on GP3, which is now wired to the CLR input of the gate array. '
               'Flash the FluxPico firmware <b>before</b> you plug the modified board in. To go back to the old '
               'firmware, Q1 has to be put back and the mod undone.', WARN))

    s.append(PageBreak())
    s.append(P('Before you start', H2))
    s.append(table([
        ['you need', 'note'],
        ['multimeter with beep (continuity)', 'to find the right pins: every step has a check'],
        ['soldering iron, thin solder', ''],
        ['small flush cutters', 'for the plug pins and the gate array leg'],
        ['thin insulated wire, about 50 cm', 'wire-wrap wire (0.25 mm) is ideal; three colours help'],
        ['tweezers', 'to take Q1 off'],
        ['magnifier or phone camera', 'take a close photo and zoom in on the screen to check your work']],
        [70 * mm, 94 * mm]))
    s.append(Spacer(1, 8))
    s.append(P('Work with the drive <b>unplugged</b>. Remove the Pico from its socket while you work on the rePico '
               'board. Take photos before you change anything.', NOTE))
    s.append(P('If something does not match a picture or a meter check: stop and send a photo. A wrong pin '
               'is easy to fix before soldering and hard after.', NOTE))
    s.append(P('Step 1: find the gate array', H2))
    (p1, (w1, h1)), (p2, (w2, h2)) = annotated_photos()
    s.append(P('Pull the rePico board out of the VIA socket. The gate array is the 40-pin chip printed '
               '<b>251828-03</b> (U10), right next to the VIA socket (U8). On this board it is soldered in, '
               'not in a socket.', BODY))

    s.append(PageBreak())
    s.append(Image(p1, width=120 * mm, height=120 * mm * h1 / w1))
    s.append(P('Figure 1: your 1541-II board, as in your photo (the board print is upside down here). Red: the '
               'gate array. Blue: the VIA socket the rePico plugs into.', CAP))

    s.append(PageBreak())
    s.append(Image(p2, width=165 * mm, height=165 * mm * h2 / w2))
    s.append(P('Figure 2: zoom of your photo, same orientation. The numbers are pin numbers. Gate array: pin 1 is '
               'top right, pins 1-20 run to the LEFT along the top row, pins 21-40 run to the RIGHT along the '
               'bottom row (the row facing the VIA socket). <b>Pin 31 (red) is the 11th leg from the left in the '
               'bottom row.</b>', CAP))
    s.append(P('In your photo the print on the chips is upside down, so pin 1 is at the top right. If you look at '
               'the board from the other side (print readable), everything is turned around: then pin 1 is bottom '
               'left and pin 31 is the 10th leg from the left in the top row. The meter checks on the next pages '
               'tell you for sure.', NOTE))

    s.append(PageBreak())
    marks = {19: (C_A, 'OE / MODE  (wire A)'), 39: (C_B, 'SOE  (wire B)'), 31: (C_CUT, 'CLR  (cut, wire C)'),
             30: (C_5V, '+5V (check)'), 32: (C_GND, 'GND (check)'), 10: (C_GND, 'GND (check)'),
             40: (C_CHK, 'BYTE (check)'), 17: (C_CHK, 'SYNC (check)')}
    s.append(fig_gate_array(marks, 'Gate array 251828 as a drawing, notch at the top'))
    s.append(P('Figure 3: the pins you need, as a drawing. Pin 31 sits between +5V (pin 30, below it) and GND (pin 32, above '
               'it).', CAP))

    s.append(P('Check with the meter (beep mode, drive unplugged, rePico out)', H2))
    s.append(P('The VIA socket U8 is marked in figure 2 (VIA 1, VIA 19, VIA 39). Its pins are counted the same '
               'way as the gate array: VIA 20 is the top-left hole, VIA 21 the bottom-left, VIA 40 the '
               'bottom-right. Each line must beep:', BODY))
    s.append(table([
        ['gate array pin', 'beeps to', 'it proves'],
        ['10', 'VIA socket pin 1 (GND)', 'the pins 1-20 are counted right'],
        ['17', 'VIA socket pin 17', 'SYNC'],
        ['19', 'VIA socket pin 19', 'MODE: here goes wire A'],
        ['30', 'VIA socket pin 20 (+5V)', 'the neighbour of 31 (one side)'],
        ['32', 'VIA socket pin 1 (GND)', 'the neighbour of 31 (other side)'],
        ['39', 'VIA socket pin 39', 'SOE: here goes wire B'],
        ['40', 'VIA socket pin 40', 'BYTE READY']],
        [40 * mm, 64 * mm, 60 * mm]))
    s.append(Spacer(1, 8))
    s.append(P('All seven beep? Then pin 31 is the one between pin 30 (+5V) and pin 32 (GND). If the chip has '
               '42 pins instead of 40, or a check fails: stop and send a photo, the numbers are then different.',
               NOTE))
    s.append(P('<b>One more check before you cut:</b> pin 31 to pin 30 (+5V): <b>no beep</b>. Pin 31 to pin 32 '
               '(GND): <b>no beep</b>. If pin 31 beeps to either, it is the wrong leg: stop and send a photo.',
               BODY))

    s.append(P('Step 2: disconnect gate array pin 31', H2))
    s.append(P('Pin 31 (CLR) gets the read pulses from the read amplifier U12. From now on it gets them from the '
               'Pico instead, so its connection to the board has to go.', BODY))
    s.append(fig_clip_leg())
    s.append(P('Figure 4: side view of three legs of the gate array, right side. Only the middle leg (31) is cut.',
               CAP))
    s.append(P('1. Cut leg 31 with the flush cutters <b>close to the board</b>, so most of the leg stays on the chip.',
               STEP))
    s.append(P('2. Bend the leg on the chip a little outwards with tweezers, so it can not touch the stub.', STEP))
    s.append(P('3. Solder <b>wire C</b> to the leg on the chip.', STEP))
    s.append(P('4. Check: leg 31 (on the chip) to the stub on the board: <b>no beep</b>. Leg 31 to leg 30 and to '
               'leg 32: no beep.', STEP))
    s.append(P('If the gate array sits in a socket: pull the chip, bend leg 31 outwards so it stays outside the '
               'socket, put the chip back and solder wire C to that leg. No cutting needed.', NOTE))

    s.append(PageBreak())
    s.append(P('Step 3: clip two plug pins on the rePico', H2))
    s.append(P('The rePico plugs into the VIA socket with two rows of pins, <b>P1</b> (VIA pins 1-20) and '
               '<b>P2</b> (VIA pins 21-40). One pin of each row (red <b>CLIP</b> in the photos) must no longer '
               'reach the socket: one ties MODE to GND, the other carries the Pico\'s copy of SOE. Wires A and B '
               'take their place.', BODY))
    tp, (tw, th) = photo_top()
    s.append(Image(tp, width=120 * mm, height=120 * mm * th / tw))
    s.append(P('Figure 5: your rePico, TOP side (VIA chip and Pico). Turn it over like the page of a book, '
               'left to right: then the Pico is on the LEFT, as in figure 6. The USB socket of the Pico stays at '
               'the top.', CAP))

    s.append(PageBreak())
    bp, (bw, bh) = photo_bottom()
    s.append(Image(bp, width=150 * mm, height=150 * mm * bh / bw))
    s.append(P('Figure 6: your rePico, BOTTOM side, Pico socket on the left. Red: the two plug pins to clip (CLIP) and Q1. '
               'Blue, orange, purple: where wires A, B and C start. The long gold pins are the plug pins: their '
               'black plastic strip sits on the board.', CAP))

    s.append(PageBreak())
    zp, (zw, zh) = photo_zoom_q1()
    s.append(Image(zp, width=165 * mm, height=165 * mm * zh / zw))
    s.append(P('Figure 7: zoom of figure 6, top end. CLIP: the second plug pin from the top, left row. Q1 is the '
               'lowest of the three small transistors.', CAP))
    zp, (zw, zh) = photo_zoom_p2()
    s.append(Image(zp, width=150 * mm, height=150 * mm * zh / zw))
    s.append(P('Figure 8: zoom of figure 6, bottom end. CLIP: the second plug pin from the bottom, right row.', CAP))

    s.append(PageBreak())
    s.append(fig_header_clip())
    s.append(P('Figure 9: side view of a plug pin row. The CLIP pin is the second pin from the end of the row.', CAP))
    s.append(P('1. Find the two red <b>CLIP</b> pins: in the left row of plug pins the second from the <b>top</b> '
               '(figure 7), in the right row the second from the <b>bottom</b> (figure 8).', STEP))
    s.append(P('2. Check before you cut: the top CLIP pin <b>beeps to GND</b> (the pad marked GND in figure 8). '
               'The top plug pin above it beeps to the pad marked +5V in figure 7.', STEP))
    s.append(P('3. Cut each pin <b>flush with the black plastic</b>, so nothing sticks out to enter the socket.',
               STEP))

    s.append(PageBreak())
    s.append(P('Step 4: remove Q1', H2))
    s.append(P('Q1 turns the port A buffer (74LVC4245) around whenever the drive reads. In flux mode the gate '
               'array drives port A, so the buffer must always listen. With Q1 gone, a resistor (R4) holds '
               'it in "listen" mode.', BODY))
    zp, (zw, zh) = photo_zoom_q1()
    s.append(Image(zp, width=165 * mm, height=165 * mm * zh / zw))
    s.append(P('Figure 7 again: Q1 in the red frame.', CAP))
    s.append(P('Q1 is the lowest of the three small transistors between the two plug pin rows, next to the print '
               '"R13". Q6 and Q8 above it look the same: do not touch them.', BODY))
    s.append(P('1. Find Q1 (figure 7). It has two legs on one side and <b>one leg alone</b> on the other side: '
               'that one is leg 3.', STEP))
    s.append(P('2. Check: one of the two legs on the other side (leg 2) <b>beeps to GND</b> (VIA socket pin 1, '
               'figure 8). Q6 and Q8 have no leg on GND: if nothing beeps to GND, you have the wrong transistor.', STEP))
    s.append(P('3. Put a big blob of fresh solder over all three legs, so one touch of the iron melts them all. '
               'Keep the iron on the blob and push Q1 off with tweezers. Clean the pads with solder wick, so no '
               'solder joins two pads.', STEP))
    s.append(P('4. Check: the single pad (where leg 3 was) to each of the other two pads: <b>no beep</b>. '
               '(Lifting only leg 3 off its pad also works, but taking Q1 off is easier to get right.)', STEP))

    s.append(PageBreak())
    s.append(P('Step 5: three wires', H2))
    s.append(fig_wiring())
    s.append(P('Figure 10: the three wires.', CAP))
    s.append(table([
        ['wire', 'on the rePico (bottom side, figures 6-8)', 'on the gate array'],
        ['A (MODE)', 'pad marked "wire A" (figure 7), not the clipped pin', 'pin 19'],
        ['B (SOE)', 'pad marked "wire B" (figure 8)', 'pin 39'],
        ['C (flux)', 'pad marked "wire C" (figure 7, Pico pin 5)', 'pin 31 (the cut leg, on the chip)']],
        [26 * mm, 88 * mm, 50 * mm]))
    s.append(Spacer(1, 8))
    s.append(P('Solder to the top of the gate array legs (where they leave the chip). Keep the wires flat on the '
               'bottom of the rePico and lead them down beside the plug pins. Check that the rePico still goes '
               'fully into the socket.', BODY))

    s.append(PageBreak())
    s.append(P('Step 6: final checks (drive unplugged, Pico out)', H2))
    s.append(table([
        ['measure', 'expected'],
        ['rePico pad "wire A"  ->  gate array pin 19', 'beep'],
        ['rePico pad "wire B"  ->  gate array pin 39', 'beep'],
        ['rePico pad "wire C"  ->  gate array leg 31', 'beep'],
        ['gate array leg 31  ->  its stub on the board', 'no beep'],
        ['rePico pad "wire A"  ->  GND', 'no beep (was GND before)'],
        ['wire A, B, C  ->  each other and GND', 'no beep'],
        ['+5V  ->  GND', 'no beep']],
        [110 * mm, 54 * mm]))
    s.append(Spacer(1, 10))
    s.append(P('Then put the Pico back, plug the rePico in, flash the <b>FluxPico</b> firmware and switch on. The '
               'first start runs a short calibration: the Pico tries different pulse shapes on pin 31 and watches '
               'the gate array\'s SYNC and port A. The display and the USB log show the result.', BODY))
    s.append(P('In write mode the drive\'s read/write amplifier also writes to the head: keep no disk in the '
               'mechanism, or unplug the head cable.', NOTE))
    s.append(P('Why each change is needed', H2))
    s.append(table([
        ['change', 'reason'],
        ['gate array pin 31 cut', 'the read amplifier U12 would fight the Pico\'s pulses'],
        ['first CLIP pin, wire A', 'the rePico tied MODE to GND, which kept the gate array switched off'],
        ['second CLIP pin, wire B', 'the gate array needs the real SOE from the VIA; it frees the Pico pin GP3'],
        ['Q1 removed', 'the port A buffer must only listen; the gate array drives port A now'],
        ['wire C from Pico GP3', 'the flux pulses (3.3 V is enough for the gate array input)']],
        [52 * mm, 112 * mm]))

    doc = SimpleDocTemplate(OUT, pagesize=A4, leftMargin=20 * mm, rightMargin=20 * mm, topMargin=15 * mm,
                            bottomMargin=16 * mm, title='1541-FluxPico: flux mod for the 1541-II',
                            author='1541-FluxPico')

    def footer(canvas, doc_):
        canvas.saveState()
        canvas.setFont('Helvetica', 9)
        canvas.setFillColor(colors.HexColor('#555555'))
        canvas.drawString(20 * mm, 9 * mm, '1541-FluxPico - flux mod, rePico v1.7 in a 1541-II')
        canvas.drawRightString(A4[0] - 20 * mm, 9 * mm, 'page %d' % doc_.page)
        canvas.restoreState()

    doc.build(s, onFirstPage=footer, onLaterPages=footer)
    print('wrote', OUT)


if __name__ == '__main__':
    build()
