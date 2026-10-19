#!/usr/bin/env python3
"""make-icon.py COASTLINE.geojson > superscan.svg

Superscan's icon: a globe lit from behind, after the "return to the past"
globe of Code Lyoko: Asia and Australia in glowing outline on an
orthographic globe, a faint graticule, two orbits, and light fanning out
from behind it. All white, in degrees of alpha, as Start's glyphs are (ztapps
keeps an all-white icon's alpha as its shape).

COASTLINE is Natural Earth's ne_110m_coastline.geojson (public domain).
"""
import json, math, sys

S = 150.0                 # viewBox
CX, CY, R = 75.0, 75.0, 48.0
LON0, LAT0, ROLL = 118.0, 8.0, -18.0   # centre of the view, and its tilt

def project(lon, lat):
    """orthographic: (x, y, visible)"""
    l, p = math.radians(lon - LON0), math.radians(lat)
    p0 = math.radians(LAT0)
    cosc = math.sin(p0) * math.sin(p) + math.cos(p0) * math.cos(p) * math.cos(l)
    x = math.cos(p) * math.sin(l)
    y = math.cos(p0) * math.sin(p) - math.sin(p0) * math.cos(p) * math.cos(l)
    r = math.radians(ROLL)
    x, y = x * math.cos(r) - y * math.sin(r), x * math.sin(r) + y * math.cos(r)
    return CX + R * x, CY - R * y, cosc > 0.02

def polylines(points):
    """the visible runs of a line of (lon, lat), as path data"""
    runs, cur = [], []
    for lon, lat in points:
        x, y, vis = project(lon, lat)
        if vis:
            cur.append((x, y))
        elif cur:
            runs.append(cur)
            cur = []
    if cur:
        runs.append(cur)
    out = []
    for run in runs:
        if len(run) < 2:
            continue
        out.append("M" + " L".join("%.2f %.2f" % p for p in run))
    return " ".join(out)

def densify(points, step=1.0):
    out = []
    for (a, b), (c, d) in zip(points, points[1:]):
        n = max(1, int(max(abs(c - a), abs(d - b)) / step))
        out += [(a + (c - a) * i / n, b + (d - b) * i / n) for i in range(n)]
    out.append(points[-1])
    return out

coast = []
for f in json.load(open(sys.argv[1]))["features"]:
    g = f["geometry"]
    lines = g["coordinates"] if g["type"] == "MultiLineString" else [g["coordinates"]]
    for line in lines:
        d = polylines(densify([tuple(p) for p in line], 0.5))
        if d:
            coast.append(d)

grat = []
for lon in range(-180, 180, 30):
    grat.append(polylines([(lon, lat) for lat in range(-90, 91, 2)]))
for lat in range(-60, 61, 30):
    grat.append(polylines([(lon, lat) for lon in range(-180, 181, 2)]))
grat = [g for g in grat if g]

# light fanning out from behind the globe all round, longest and brightest
# up and to the right as in the picture, and kept inside the square (so that
# ztapps, which crops a glyph to its shape, keeps the globe its size)
rays = []
ox, oy = CX + 4, CY - 8
for i in range(26):
    deg = i * (360 / 26) + (4 if i % 2 else 0)
    a = math.radians(deg)
    up = max(0.0, -math.sin(a)) * 0.6 + max(0.0, math.cos(a - math.radians(-40))) * 0.4
    spread = math.radians(1.2 + (i % 3) * 0.8 + up * 1.2)
    L = 58 + up * 14 + (i * 37 % 5)
    p1 = (ox + L * math.cos(a - spread), oy + L * math.sin(a - spread))
    p2 = (ox + L * math.cos(a + spread), oy + L * math.sin(a + spread))
    rays.append("M%.1f %.1f L%.1f %.1f L%.1f %.1f Z" % (ox, oy, *p1, *p2))

def orbit(tilt, rx, ry, start, end):
    """part of a tilted ellipse round the globe"""
    t = math.radians(tilt)
    pts = []
    for k in range(start, end + 1, 3):
        u = math.radians(k)
        x, y = rx * math.cos(u), ry * math.sin(u)
        pts.append((CX + x * math.cos(t) - y * math.sin(t), CY + x * math.sin(t) + y * math.cos(t)))
    return "M" + " L".join("%.2f %.2f" % p for p in pts)

orbits = [orbit(-32, R * 1.32, R * 0.42, 170, 380), orbit(24, R * 1.22, R * 0.30, -20, 200)]

print(f'''<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {S:.0f} {S:.0f}" width="150" height="150">
  <!-- Superscan: a globe lit from behind (made by icon/make-icon.py) -->
  <defs>
    <radialGradient id="light" gradientUnits="userSpaceOnUse" cx="{CX + 4}" cy="{CY - 8}" r="72">
      <stop offset="0" stop-color="#fff" stop-opacity="0.55"/>
      <stop offset="0.45" stop-color="#fff" stop-opacity="0.22"/>
      <stop offset="1" stop-color="#fff" stop-opacity="0"/>
    </radialGradient>
    <radialGradient id="sphere" cx="0.45" cy="0.38" r="0.6">
      <stop offset="0" stop-color="#fff" stop-opacity="0.22"/>
      <stop offset="1" stop-color="#fff" stop-opacity="0.04"/>
    </radialGradient>
    <filter id="glow" x="-20%" y="-20%" width="140%" height="140%">
      <feGaussianBlur stdDeviation="1.6"/>
    </filter>
  </defs>
  <clipPath id="square"><rect x="2" y="2" width="{S - 4:.0f}" height="{S - 4:.0f}"/></clipPath>
  <path fill="url(#light)" clip-path="url(#square)" d="{' '.join(rays)}"/>
  <circle cx="{CX}" cy="{CY}" r="{R}" fill="url(#sphere)"/>
  <g fill="none" stroke="#fff" stroke-linecap="round" stroke-linejoin="round">
    <path stroke-opacity="0.32" stroke-width="0.7" d="{' '.join(grat)}"/>
    <path stroke-opacity="0.55" stroke-width="0.9" d="{' '.join(orbits)}"/>
    <circle cx="{CX}" cy="{CY}" r="{R}" stroke-opacity="0.75" stroke-width="1.4"/>
    <path stroke-opacity="0.9" stroke-width="3.2" filter="url(#glow)" d="{' '.join(coast)}"/>
    <path stroke-width="1.5" d="{' '.join(coast)}"/>
  </g>
</svg>''')
