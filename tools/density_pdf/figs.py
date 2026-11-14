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
                  parts=('IC1', 'P1', 'U1', 'R19', 'Q9', 'J8', 'BZ1')):
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
        names = {'IC1': 'IC1 VIA socket', 'P1': 'P1', 'U1': 'U1 Pico', 'J8': 'J8', 'BZ1': 'BZ1'}
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
        tw = len(text) * 3.75 + 4
        left = tx if dx > 0 else tx - tw
        d.add(Rect(left - 1, ty - 3.5, tw + 2, 10, fillColor=colors.white, strokeColor=col, strokeWidth=0.5))
        d.add(String(left + 1, ty - 0.3, text, fontName='Helvetica-Bold', fontSize=7, fillColor=col))
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
J_DS1 = (136.4, 77.33)                  # junction of the DS1 divider (between the P1 and U1 pin rows)


def wires_ds1(d, v):
    x16, y16 = _xy('IC1', 16)
    x5, y5 = _xy('U1', 5)
    x3, y3 = _xy('U1', 3)
    path(d, v, [(x16, y16), (x16, J_DS1[1]), J_DS1, (x5, J_DS1[1]), (x5, y5)], C_DS1, res=(1, '10k'))
    path(d, v, [J_DS1, (J_DS1[0], y3), (x3, y3)], C_DS1, res=(0, '22k'))
    dot(d, v, J_DS1[0], J_DS1[1], C_DS1)


def wire_gnd_link(d, v):
    xr, yr = _xy('R19', 2)
    x3, y3 = _xy('U1', 3)
    path(d, v, [(xr, yr), (xr, y3), (x3, y3)], C_GND, dash=(1.5, 1.5))


def wires_ds0(d, v):
    x15, y15 = _xy('IC1', 15)
    xj2, yj2 = _xy('J8', 2)
    xj3, yj3 = _xy('J8', 3)
    xm = 121.8                           # inside the VIA socket, between its two pin rows
    path(d, v, [(x15, y15), (xm, y15), (xm, 120.4), (xj2, 120.4), (xj2, yj2)], C_DS0, res=(1, '10k'))
    path(d, v, [(xj2, yj2 - 1.6), (xj3, yj3 - 1.6)], C_DS0, res=(0, '22k'), dash=None)


def fig_bottom_full():
    bx0, by0, bx1, by1 = B.outline
    v = View(bx0, by0, bx1, by1, 160 * mm, bottom=True)
    labels = [('IC1', 15, C_DS0, 'VIA pin 15 = DS0', 40, -30),
              ('IC1', 16, C_DS1, 'VIA pin 16 = DS1', 40, 26),
              ('U1', 5, C_DS1, 'Pico pin 5 = GPIO3', -70, -40),
              ('U1', 3, C_GND, 'Pico pin 3 = GND', -60, 24),
              ('J8', 2, C_DS0, 'J8 pin 2 = GPIO17', -95, 36),
              ('J8', 3, C_GND, 'J8 pin 3 = GND', -80, -20)]

    def extra(d, vv):
        wires_ds0(d, vv)
        wires_ds1(d, vv)
        wire_gnd_link(d, vv)
        cut_mark(d, *vv.p(*CUT_AT))
    return board_drawing(v, 'B.Cu', labels, extra,
                         title='Board v1.7, BOTTOM side (seen from below; drawn from the KiCad file)')


def fig_bottom_zoom():
    v = View(126.5, 71.0, 148.5, 88.0, 160 * mm, bottom=True)
    labels = [('IC1', 16, C_DS1, 'VIA pin 16 (DS1)', 24, 40),
              ('IC1', 15, C_DS0, 'VIA pin 15 (DS0): wire to J8, see figure 4', -40, -44),
              ('U1', 5, C_DS1, 'Pico pin 5 (GPIO3)', 24, -58),
              ('U1', 3, C_GND, 'Pico pin 3 (GND)', 30, 34),
              ('R19', 2, C_SOE, 'R19 pad 2', -30, -44),
              ('Q9', 2, C_SOE, 'Q9', -24, 44)]

    def extra(d, vv):
        wires_ds1(d, vv)
        wire_gnd_link(d, vv)
        cut_mark(d, *vv.p(*CUT_AT))
    return board_drawing(v, 'B.Cu', labels, extra, title='Zoom, BOTTOM side: cut SOE_GA, DS1 to GPIO3')


def fig_top_buzzer():
    v = View(100.2, 108.0, 122.0, 125.07, 110 * mm, bottom=False)
    labels = [('BZ1', 1, C_CUT, 'BZ1: remove the buzzer', 30, 26),
              ('J8', 1, C_GND, 'J8.1 3.3V', -14, -30),
              ('J8', 2, C_DS0, 'J8.2 GPIO17', 10, -44),
              ('J8', 3, C_GND, 'J8.3 GND', 34, -26)]
    return board_drawing(v, 'F.Cu', labels, None, title='TOP side at BZ1/J8 (drawn from the KiCad file)',
                         parts=('J8', 'BZ1'))
