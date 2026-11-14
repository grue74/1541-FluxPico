"""Board drawings for the mod PDFs, made from the KiCad PCB file."""
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
