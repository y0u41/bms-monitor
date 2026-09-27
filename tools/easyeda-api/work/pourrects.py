"""Regenerate _pour_rects.json from the live pour polygons (EDA bridge).

audit.py needs orthogonal rectangles whose union equals each pour outline, so
plane connectivity can be seen; exact x-slab decomposition, no bounding boxes.
"""
import json, io, subprocess, sys

PAYLOAD = ('{"code":"const list=await eda.pcb_PrimitivePour.getAll(); const out=[];'
           'for(const p of list){ out.push({net:p.net, layer:p.layer, id:p.primitiveId,'
           ' name:p.pourName||null, poly:typeof p.complexPolygon===\'string\''
           '?p.complexPolygon:JSON.stringify(p.complexPolygon)}); } return JSON.stringify(out);"}')


def fetch():
    io.open('_payload.json', 'w', encoding='utf-8').write(PAYLOAD)
    raw = subprocess.run(['curl', '-s', '--max-time', '60', '-X', 'POST',
                          'http://127.0.0.1:49620/execute',
                          '-H', 'Content-Type: application/json', '-d@_payload.json'],
                         capture_output=True, text=True, encoding='utf-8').stdout
    return json.loads(json.loads(raw)['result'])


def parse(poly):
    toks = json.loads(poly)['polygon'] if isinstance(poly, str) else poly['polygon']
    pts, i = [], 1 if isinstance(toks[1], str) else 0
    j = i
    while j < len(toks):
        if isinstance(toks[j], str):
            j += 1
            continue
        pts.append((toks[j], toks[j + 1]))
        j += 2
    return pts


def inside(pts, x, y):
    hit = False
    n = len(pts)
    for k in range(n):
        (x1, y1), (x2, y2) = pts[k], pts[(k + 1) % n]
        if (y1 > y) != (y2 > y):
            if x < x1 + (y - y1) * (x2 - x1) / (y2 - y1):
                hit = not hit
    return hit


def rects(pts):
    if pts[0] == pts[-1]:
        pts = pts[:-1]
    xs = sorted({p[0] for p in pts})
    out = []
    n = len(pts)
    for a, b in zip(xs, xs[1:]):
        xm = (a + b) / 2.0
        ys = []
        for k in range(n):
            (x1, y1), (x2, y2) = pts[k], pts[(k + 1) % n]
            if y1 == y2 and min(x1, x2) <= a and max(x1, x2) >= b:
                ys.append(y1)
        ys = sorted(set(ys))
        for y1, y2 in zip(ys, ys[1:]):
            if inside(pts, xm, (y1 + y2) / 2.0):
                out.append((a, y1, b, y2))
    merged = []
    for r in sorted(out):
        if merged and merged[-1][0] == r[0] and merged[-1][2] == r[2] and merged[-1][3] == r[1]:
            x1, y1, x2, _ = merged[-1]
            merged[-1] = (x1, y1, x2, r[3])
        else:
            merged.append(r)
    return merged


def main():
    out = []
    for p in fetch():
        for (x1, y1, x2, y2) in rects(parse(p['poly'])):
            out.append({'net': p['net'], 'layer': p['layer'],
                        'x1': x1, 'y1': y1, 'x2': x2, 'y2': y2, 'pour': p['name']})
    io.open('_pour_rects.json', 'w', encoding='utf-8').write(json.dumps(out, indent=1))
    area = sum((r['x2'] - r['x1']) * (r['y2'] - r['y1']) for r in out)
    print('pours=%d rects=%d area_mm2=%.1f' %
          (len(set(r['pour'] for r in out)), len(out), area / 1600.0))


main()
