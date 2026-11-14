"""Read footprints, pads, tracks and the outline from the KiCad PCB file.

All positions are absolute board coordinates in mm (KiCad: x right, y down).
"""
import math
import re

PCB = r'E:\GitHub\1541-rePico\hardware\1541-rePico.kicad_pcb'


def _rot(x, y, deg):
    # KiCad rotates counter-clockwise on screen (y axis points down)
    a = math.radians(deg)
    return x * math.cos(a) + y * math.sin(a), -x * math.sin(a) + y * math.cos(a)


class Board:
    def __init__(self, path=PCB):
        t = open(path, encoding='utf-8').read()
        self.footprints = {}
        for m in re.finditer(r'\n\t\(footprint "([^"]*)"(.*?)\n\t\)', t, re.S):
            body = m.group(2)
            ref = re.search(r'\(property "Reference" "([^"]*)"', body)
            val = re.search(r'\(property "Value" "([^"]*)"', body)
            at = re.search(r'\n\t\t\(at ([\d.\-]+) ([\d.\-]+)(?: ([\d.\-]+))?\)', body)
            layer = re.search(r'\(layer "([^"]*)"\)', body).group(1)
            fx, fy, fr = float(at.group(1)), float(at.group(2)), float(at.group(3) or 0)
            pads = []
            for p in re.finditer(r'\(pad "(\w*)" (\w+) (\w+)(.*?)(?=\(pad |\Z)', body, re.S):
                pa = re.search(r'\(at ([\d.\-]+) ([\d.\-]+)', p.group(4))
                sz = re.search(r'\(size ([\d.\-]+) ([\d.\-]+)\)', p.group(4))
                net = re.search(r'\(net \d+ "([^"]*)"\)', p.group(4))
                dx, dy = _rot(float(pa.group(1)), float(pa.group(2)), fr)
                pads.append(dict(num=p.group(1), kind=p.group(2), shape=p.group(3),
                                 x=fx + dx, y=fy + dy,
                                 w=float(sz.group(1)) if sz else 1.0, h=float(sz.group(2)) if sz else 1.0,
                                 net=net.group(1) if net else ''))
            name = ref.group(1) if ref else m.group(1)
            self.footprints[name] = dict(lib=m.group(1), value=val.group(1) if val else '',
                                         layer=layer, x=fx, y=fy, rot=fr, pads=pads)
        self.segments = []
        nets = dict((n, i) for i, n in re.findall(r'\(net (\d+) "([^"]*)"\)', t))
        names = dict((i, n) for n, i in nets.items())
        for s in re.finditer(r'\(segment\s+\(start ([\d.\-]+) ([\d.\-]+)\)\s+\(end ([\d.\-]+) ([\d.\-]+)\)\s+'
                             r'\(width ([\d.]+)\)\s+\(layer "([^"]*)"\)\s+\(net (\d+)\)', t):
            self.segments.append(dict(x1=float(s.group(1)), y1=float(s.group(2)), x2=float(s.group(3)),
                                      y2=float(s.group(4)), w=float(s.group(5)), layer=s.group(6),
                                      net=names.get(s.group(7), '')))
        xs, ys = [], []
        for m in re.finditer(r'\(gr_(?:line|rect)\s+\(start ([\d.\-]+) ([\d.\-]+)\)\s+\(end ([\d.\-]+) ([\d.\-]+)\)'
                             r'.*?\(layer "Edge.Cuts"\)', t, re.S):
            xs += [float(m.group(1)), float(m.group(3))]
            ys += [float(m.group(2)), float(m.group(4))]
        self.outline = (min(xs), min(ys), max(xs), max(ys))

    def pad(self, ref, num):
        for p in self.footprints[ref]['pads']:
            if p['num'] == str(num):
                return p
        raise KeyError((ref, num))


if __name__ == '__main__':
    b = Board()
    for ref, num in [('IC1', 15), ('IC1', 16), ('U1', 5), ('U1', 3), ('U1', 8), ('U1', 22), ('R19', 1), ('R19', 2),
                     ('Q9', 1), ('Q9', 2), ('Q9', 3), ('J8', 1), ('J8', 2), ('J8', 3), ('BZ1', 1), ('BZ1', 2)]:
        p = b.pad(ref, num)
        print(ref, num, round(p['x'], 2), round(p['y'], 2), p['net'])
    print([s for s in b.segments if s['net'] == '/_SOE_GA'])
    print(b.outline, b.footprints['Q9']['layer'], b.footprints['R19']['layer'])
