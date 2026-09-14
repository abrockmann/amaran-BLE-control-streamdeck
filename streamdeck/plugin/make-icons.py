#!/usr/bin/env python3
"""Generates the plugin icons as PNG — without third-party libraries, so the
plugin is built entirely from sources you can read."""
import struct, zlib, pathlib, math

OUT = pathlib.Path(__file__).parent / "com.arnebrockmann.amaran.sdPlugin" / "imgs"
OUT.mkdir(parents=True, exist_ok=True)


def write_png(path, size, shader):
    rows = bytearray()
    for y in range(size):
        rows.append(0)  # Filter: None
        for x in range(size):
            rows += bytes(shader(x, y, size))
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
           + chunk(b"IEND", b""))
    path.write_bytes(png)


def disc(cx, cy, r, x, y, feather=1.2):
    """Soft edge mask of a circle, 0..1."""
    d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
    return max(0.0, min(1.0, (r - d) / feather))


def sun(x, y, s):
    c, r = s / 2, s * 0.26
    a = disc(c, c, r, x, y)
    # eight rays
    ang = math.atan2(y + 0.5 - c, x + 0.5 - c)
    dist = math.hypot(x + 0.5 - c, y + 0.5 - c)
    if s * 0.34 < dist < s * 0.46:
        k = (math.cos(ang * 8) + 1) / 2
        if k > 0.72:
            a = max(a, min(1.0, (k - 0.72) / 0.12))
    v = int(255 * a)
    return (255, 255, 255, v)


def cct(x, y, s):
    c, r = s / 2, s * 0.34
    a = disc(c, c, r, x, y)
    warm, cool = (255, 182, 92), (156, 199, 255)
    col = warm if x + 0.5 < c else cool
    return (col[0], col[1], col[2], int(255 * a))


def plug(x, y, s):
    c = s / 2
    outer = disc(c, c, s * 0.38, x, y)
    inner = disc(c, c, s * 0.24, x, y)
    ring = max(0.0, outer - inner)
    core = disc(c, c, s * 0.13, x, y)
    a = max(ring, core)
    return (255, 255, 255, int(255 * a))


def power_symbol(on):
    """Power symbol: open ring plus stem. On = warm and bright,
    off = muted gray — the state should read at a glance."""
    col = (255, 196, 110) if on else (120, 120, 120)

    def shader(x, y, s):
        c = s / 2
        dx, dy = x + 0.5 - c, y + 0.5 - c
        dist = math.hypot(dx, dy)
        r_out, r_in = s * 0.34, s * 0.26
        ring = max(0.0, min(1.0, (r_out - dist) / 1.2)) * max(0.0, min(1.0, (dist - r_in) / 1.2))
        # gap at the top of the ring, so it reads as a power symbol
        ang = math.degrees(math.atan2(dy, dx))            # -90 = top
        if abs(ang + 90) < 32:
            ring = 0.0
        # vertical stem from the top to just above the center
        half = s * 0.045
        stem = 0.0
        if -s * 0.40 < dy < -s * 0.02:
            stem = max(0.0, min(1.0, (half - abs(dx)) / 1.0))
        a = max(ring, stem)
        return (col[0], col[1], col[2], int(255 * a))
    return shader


def cover(sd):
    """Coverage of a pixel from its distance to the edge (negative = inside)."""
    return max(0.0, min(1.0, 0.5 - sd))


def ellipse_sd(dx, dy, rx, ry):
    """Distance to an ellipse's edge in pixels (first-order approximation)."""
    f = (dx / rx) ** 2 + (dy / ry) ** 2 - 1.0
    g = math.hypot(2 * dx / rx ** 2, 2 * dy / ry ** 2) or 1e-9
    return f / g


def box_sd(dx, dy, hw, hh, r):
    """Distance to the edge of a rounded rectangle around the origin."""
    qx, qy = abs(dx) - hw + r, abs(dy) - hh + r
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r


def over(dst, rgb, a):
    """Composite a color with coverage a over a pixel (straight alpha)."""
    da = dst[3]
    oa = a + da * (1 - a)
    if oa <= 0:
        return (0.0, 0.0, 0.0, 0.0)
    mix = lambda i: (rgb[i] * a + dst[i] * da * (1 - a)) / oa
    return (mix(0), mix(1), mix(2), oa)


def lamp(on):
    """The Verge Max as a pictogram: round disc with a frame on a small foot.
    Drawn here — it only describes the light's shape, no artwork from the
    manufacturer. On: warm surface with a glow. Off: dark surface."""
    frame = (205, 205, 205) if on else (118, 118, 118)
    warm_mid, warm_edge = (255, 232, 184), (255, 186, 104)
    dark = (46, 46, 46)
    glow = (255, 190, 110)

    def shader(x, y, s):
        cx, cy = s / 2, s * 0.42
        rx = ry = s * 0.33
        t = max(1.5, s * 0.042)                     # frame thickness
        dx, dy = x + 0.5 - cx, y + 0.5 - cy
        outer = ellipse_sd(dx, dy, rx, ry)
        inner = ellipse_sd(dx, dy, rx - t, ry - t)

        px = (0.0, 0.0, 0.0, 0.0)
        if on and outer > 0:                        # glow outside the frame
            px = over(px, glow, 0.38 * math.exp(-outer / (s * 0.045)))

        # foot: short neck under the disc, a flat plate below it
        neck = box_sd(dx, y + 0.5 - (cy + ry + s * 0.025), s * 0.028, s * 0.035, 0.5)
        base = box_sd(dx, y + 0.5 - (cy + ry + s * 0.070), s * 0.120, s * 0.022, s * 0.012)
        px = over(px, frame, cover(min(neck, base)))

        px = over(px, frame, cover(outer))          # frame (whole disc)
        if on:                                      # diffuser, brighter towards the center
            k = max(0.0, min(1.0, math.hypot(dx / (rx - t), dy / (ry - t))))
            k = k ** 1.6
            fill = tuple(warm_mid[i] + (warm_edge[i] - warm_mid[i]) * k for i in range(3))
        else:
            fill = dark
        px = over(px, fill, cover(inner))
        return tuple(int(round(v)) for v in px[:3]) + (int(round(255 * px[3])),)
    return shader


for name, shader in (("brightness", sun), ("cct", cct), ("plugin", plug),
                     ("power-off", power_symbol(False)), ("power-on", power_symbol(True)),
                     ("verge-off", lamp(False)), ("verge-on", lamp(True))):
    write_png(OUT / f"{name}.png", 72, shader)
    write_png(OUT / f"{name}@2x.png", 144, shader)
    print("generated:", name + ".png", "+ @2x")
