"""Board figures for the density-select PDF, drawn from the KiCad data."""
from reportlab.graphics.shapes import Circle, Drawing, Line, Polygon, Rect, String
from reportlab.lib import colors
from reportlab.lib.units import mm

from pcb import Board

B = Board()

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


class View:
    """maps board mm to drawing points; the bottom view is mirrored left-right"""

    def __init__(self, x0, y0, x1, y1, width_pt, bottom):
        self.x0, self.y0, self.x1, self.y1 = x0, y0, x1, y1
        self.s = width_pt / (x1 - x0)
        self.w = width_pt
        self.h = (y1 - y0) * self.s
        self.bottom = bottom

    def p(self, x, y):
        px = (self.x1 - x) if self.bottom else (x - self.x0)
        return px * self.s, (self.y1 - y) * self.s

    def inside(self, x, y, margin=0.0):
        return (self.x0 + margin) <= x <= (self.x1 - margin) and (self.y0 + margin) <= y <= (self.y1 - margin)


def _clip(x1, y1, x2, y2, w, h):
    """Liang-Barsky clipping of a line to 0..w, 0..h; None if outside"""
    dx, dy = x2 - x1, y2 - y1
    t0, t1 = 0.0, 1.0
    for p, q in ((-dx, x1), (dx, w - x1), (-dy, y1), (dy, h - y1)):
        if p == 0:
            if q < 0:
                return None
            continue
        t = q / p
        if p < 0:
            t0 = max(t0, t)
        else:
            t1 = min(t1, t)
        if t0 > t1:
            return None
    return x1 + t0 * dx, y1 + t0 * dy, x1 + t1 * dx, y1 + t1 * dy


def _xy(ref, num):
    p = B.pad(ref, num)
    return p['x'], p['y']


def board_drawing(v, layer, pad_labels=(), extra=None, title=None,
                  parts=('IC1', 'P1', 'U1', 'R19', 'Q9', 'J8', 'BZ1'), lsize=7):
    d = Drawing(v.w, v.h + (14 if title else 0))
    bx0, by0, bx1, by1 = B.outline
    ax, ay = v.p(max(bx0, v.x0), min(by1, v.y1))
    bx, byy = v.p(min(bx1, v.x1), max(by0, v.y0))
    d.add(Rect(min(ax, bx), min(ay, byy), abs(bx - ax), abs(byy - ay), fillColor=C_BOARD,
               strokeColor=C_EDGE, strokeWidth=1))
    for s in B.segments:
        if s['layer'] != layer:
            continue
        x1, y1 = v.p(s['x1'], s['y1'])
        x2, y2 = v.p(s['x2'], s['y2'])
        c = _clip(x1, y1, x2, y2, v.w, v.h)
        if not c:
            continue
        col = C_SOE if s['net'] == '/_SOE_GA' else C_TRACK
        d.add(Line(c[0], c[1], c[2], c[3], strokeColor=col, strokeWidth=max(0.6, s['w'] * v.s)))
    for ref, fp in B.footprints.items():
        for p in fp['pads']:
            if not v.inside(p['x'], p['y'], max(p['w'], p['h']) / 2):
                continue
            if p['kind'] == 'smd' and fp['layer'] != layer:
                continue
            x, y = v.p(p['x'], p['y'])
            if p['kind'] == 'smd':
                w, h = p['w'] * v.s, p['h'] * v.s
                d.add(Rect(x - w / 2, y - h / 2, w, h, fillColor=C_PAD, strokeColor=None))
            else:
                r = max(1.2, min(p['w'], p['h']) * v.s / 2)
                d.add(Circle(x, y, r, fillColor=C_PAD, strokeColor=colors.HexColor('#8A8A8A'), strokeWidth=0.3))
    for ref in parts:
        fp = B.footprints[ref]
        if fp['layer'] != layer and all(p['kind'] == 'smd' for p in fp['pads']):
            continue
        xs = [p['x'] for p in fp['pads']]
        ys = [p['y'] for p in fp['pads']]
        m = 1.4 if ref not in ('R19', 'Q9') else 0.9
        xa, ya = v.p(min(xs) - m, max(ys) + m)
        xb, yb = v.p(max(xs) + m, min(ys) - m)
        rx0, rx1 = max(0, min(xa, xb)), min(v.w, max(xa, xb))
        ry0, ry1 = max(0, min(ya, yb)), min(v.h, max(ya, yb))
        if rx0 >= rx1 or ry0 >= ry1:
            continue
        d.add(Rect(rx0, ry0, rx1 - rx0, ry1 - ry0, fillColor=None,
                   strokeColor=colors.HexColor('#6A8F6A'), strokeWidth=0.6, strokeDashArray=[2, 2]))
        names = {'IC1': 'IC1 VIA socket', 'P1': 'P1', 'P2': 'P2', 'U1': 'U1 Pico', 'J8': 'J8', 'BZ1': 'BZ1',
                 'Q1': 'Q1'}
        if ref in names and (ry1 - ry0) > 40:
            d.add(String((rx0 + rx1) / 2, ry0 + 3, names[ref], fontName='Helvetica', fontSize=6.5,
                         fillColor=colors.HexColor('#4A6F4A'), textAnchor='middle'))
    if extra:
        extra(d, v)
    for ref, num, col, text, dx, dy in pad_labels:
        x, y = v.p(*_xy(ref, num))
        p = B.pad(ref, num)
        r = max(2.6, min(p['w'], p['h']) * v.s / 2 + 1.2)
        d.add(Circle(x, y, r, fillColor=None, strokeColor=col, strokeWidth=1.6))
        tx, ty = x + dx, y + dy
        d.add(Line(x, y, tx, ty, strokeColor=col, strokeWidth=0.6))
        tw = len(text) * lsize * 0.56 + 4
        left = tx if dx > 0 else tx - tw
        d.add(Rect(left - 1, ty - lsize * 0.5, tw + 2, lsize * 1.45, fillColor=colors.white, strokeColor=col,
                   strokeWidth=0.5 + lsize / 14))
        d.add(String(left + 1, ty - 0.3, text, fontName='Helvetica-Bold', fontSize=lsize, fillColor=col))
    if title:
        d.add(String(v.w / 2, v.h + 4, title, fontName='Helvetica-Bold', fontSize=9, fillColor=C_TEXT,
                     textAnchor='middle'))
    return d


def resistor(d, x1, y1, x2, y2, col, label):
    """wire with a resistor box in the middle"""
    mx, my = (x1 + x2) / 2, (y1 + y2) / 2
    dx, dy = x2 - x1, y2 - y1
    ln = (dx * dx + dy * dy) ** 0.5 or 1
    ux, uy = dx / ln, dy / ln
    bl = min(18, ln * 0.6)
    ax, ay = mx - ux * bl / 2, my - uy * bl / 2
    bx, by = mx + ux * bl / 2, my + uy * bl / 2
    d.add(Line(x1, y1, ax, ay, strokeColor=col, strokeWidth=1.4))
    d.add(Line(bx, by, x2, y2, strokeColor=col, strokeWidth=1.4))
    nx, ny = -uy * 3.4, ux * 3.4
    d.add(Polygon([ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny, ax - nx, ay - ny],
                  fillColor=colors.white, strokeColor=col, strokeWidth=1.3))
    if abs(uy) < 0.5:
        d.add(String(mx, my + 6, label, fontName='Helvetica-Bold', fontSize=7, fillColor=col, textAnchor='middle'))
    else:
        d.add(String(mx + 6, my - 2.5, label, fontName='Helvetica-Bold', fontSize=7, fillColor=col))


def path(d, v, pts, col, res=None, dash=(3, 2)):
    """new wire through board points; res = (segment index, label) puts a resistor on that segment"""
    q = [v.p(x, y) for x, y in pts]
    for i in range(len(q) - 1):
        (x1, y1), (x2, y2) = q[i], q[i + 1]
        if res and res[0] == i:
            resistor(d, x1, y1, x2, y2, col, res[1])
        else:
            d.add(Line(x1, y1, x2, y2, strokeColor=col, strokeWidth=1.4,
                       strokeDashArray=list(dash) if dash else None))


def dot(d, v, x, y, col):
    px, py = v.p(x, y)
    d.add(Circle(px, py, 2.2, fillColor=col, strokeColor=None))


def cut_mark(d, x, y):
    for k in (-1.8, 1.8):
        d.add(Line(x + k, y - 8, x + k, y + 8, strokeColor=C_CUT, strokeWidth=1.6))
    d.add(Rect(x - 11, y + 10, 22, 9, fillColor=colors.white, strokeColor=C_CUT, strokeWidth=0.6))
    d.add(String(x, y + 12, 'CUT', fontName='Helvetica-Bold', fontSize=7.5, fillColor=C_CUT, textAnchor='middle'))


CUT_AT = (140.2, 78.6)                  # on the SOE_GA trace between Pico pin 5 and R19
C_5V = colors.HexColor('#B02060')


def wire_gnd_link(d, v):
    xr, yr = _xy('R19', 2)
    x3, y3 = _xy('U1', 3)
    path(d, v, [(xr, yr), (xr, y3), (x3, y3)], C_GND, dash=(1.5, 1.5))


def fig_bottom_full():
    bx0, by0, bx1, by1 = B.outline
    v = View(bx0, by0, bx1, by1, 160 * mm, bottom=True)
    labels = [('IC1', 15, C_DS0, 'VIA pin 15 = DS0 -> HV1', 40, -30),
              ('IC1', 16, C_DS1, 'VIA pin 16 = DS1 -> HV2', 40, 26),
              ('IC1', 20, C_5V, 'VIA pin 20 = +5V -> HV', 70, -6),
              ('U1', 5, C_DS1, 'Pico pin 5 = GPIO3 -> LV2', -70, -40),
              ('U1', 3, C_GND, 'Pico pin 3 = GND', -60, 24)]

    def extra(d, vv):
        wire_gnd_link(d, vv)
        cut_mark(d, *vv.p(*CUT_AT))
    return board_drawing(v, 'B.Cu', labels, extra,
                         title='Board v1.7, BOTTOM side (seen from below; drawn from the KiCad file)')


def fig_bottom_zoom():
    v = View(126.5, 71.0, 148.5, 88.0, 160 * mm, bottom=True)
    labels = [('IC1', 16, C_DS1, 'VIA pin 16 (DS1) -> HV2', 24, 40),
              ('IC1', 15, C_DS0, 'VIA pin 15 (DS0) -> HV1', -40, -44),
              ('U1', 5, C_DS1, 'Pico pin 5 (GPIO3) -> LV2', 24, -58),
              ('U1', 3, C_GND, 'Pico pin 3 (GND)', 30, 34),
              ('R19', 2, C_SOE, 'R19 pad 2', -30, -44),
              ('Q9', 2, C_SOE, 'Q9', -24, 44)]

    def extra(d, vv):
        wire_gnd_link(d, vv)
        cut_mark(d, *vv.p(*CUT_AT))
    return board_drawing(v, 'B.Cu', labels, extra, title='Zoom, BOTTOM side: cut SOE_GA, tie R19 pad 2 to GND')


def fig_module():
    """wiring of the 4-channel BSS138 level shifter module, pins as printed on it"""
    W, H = 165 * mm, 190
    d = Drawing(W, H)
    mx, my, mw, mh = W / 2 - 80, 58, 160, 74
    d.add(Rect(mx, my, mw, mh, rx=4, ry=4, fillColor=colors.HexColor('#2B4FA0'), strokeColor=C_TEXT, strokeWidth=1))
    d.add(String(mx + mw / 2, my + mh / 2 - 3, '4 x BSS138, 10k pull-ups', fontName='Helvetica', fontSize=7,
                 fillColor=colors.white, textAnchor='middle'))
    hv = [('HV1', C_DS0, 'VIA pin 15 (DS0)'), ('HV2', C_DS1, 'VIA pin 16 (DS1)'), ('HV', C_5V, 'VIA pin 20 (+5V)'),
          ('GND', C_GND, 'J8 pin 3 (GND)'), ('HV3', None, 'free'), ('HV4', None, 'free')]
    lv = [('LV1', C_DS0, 'J8 pin 2 (GPIO17)'), ('LV2', C_DS1, 'Pico pin 5 (GPIO3)'), ('LV', C_5V, 'J8 pin 1 (3.3V)'),
          ('GND', C_GND, '(already GND)'), ('LV3', None, 'free'), ('LV4', None, 'free')]
    step = mw / 6
    for items, yy, up in ((hv, my + mh - 10, True), (lv, my + 10, False)):
        sg = 1 if up else -1
        for i, (name, col, dest) in enumerate(items):
            x = mx + step * (i + 0.5)
            d.add(Circle(x, yy, 4.2, fillColor=colors.HexColor('#D8B060'), strokeColor=C_TEXT, strokeWidth=0.5))
            d.add(String(x, yy + (-13 if up else 8), name, fontName='Helvetica-Bold', fontSize=7,
                         fillColor=colors.white, textAnchor='middle'))
            if col is None:
                continue
            if name == 'LV':
                col = colors.HexColor('#2E8B57')
            # elbow heights chosen so that no two wires cross
            hy = yy + sg * (18 if i in (0, 3) else 30)
            lx = mx + mw / 2 + (i - 1.5) * 104
            ly = yy + sg * 44
            for (xa, ya, xb, yb) in ((x, yy, x, hy), (x, hy, lx, hy), (lx, hy, lx, ly)):
                d.add(Line(xa, ya, xb, yb, strokeColor=col, strokeWidth=1.4))
            tw = len(dest) * 3.7 + 6
            by = ly if up else ly - 10
            d.add(Rect(lx - tw / 2, by, tw, 10, fillColor=colors.white, strokeColor=col, strokeWidth=0.6))
            d.add(String(lx, by + 3, dest, fontName='Helvetica-Bold', fontSize=7, fillColor=col, textAnchor='middle'))
    d.add(String(mx - 8, my + mh - 13, 'HV = 5 V side', fontName='Helvetica', fontSize=7, fillColor=C_TEXT,
                 textAnchor='end'))
    d.add(String(mx - 8, my + 7, 'LV = 3.3 V side', fontName='Helvetica', fontSize=7, fillColor=C_TEXT,
                 textAnchor='end'))
    return d


def fig_top_buzzer():
    v = View(100.2, 108.0, 122.0, 125.07, 110 * mm, bottom=False)
    labels = [('BZ1', 1, C_CUT, 'BZ1: remove the buzzer', 30, 26),
              ('J8', 1, C_GND, 'J8.1 3.3V -> LV', -14, -30),
              ('J8', 2, C_DS0, 'J8.2 GPIO17 -> LV1', 10, -44),
              ('J8', 3, C_GND, 'J8.3 GND -> GND', 34, -26)]
    return board_drawing(v, 'F.Cu', labels, None, title='TOP side at BZ1/J8 (drawn from the KiCad file)',
                         parts=('J8', 'BZ1'))
