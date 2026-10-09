#!/usr/bin/env python3
"""Draws DQ8Recomp's icon, the launcher's crown on a Dragon Quest window, and
writes it in every format the executables take:

    dq8.png          1024 px, for anything else (a README, a .desktop file)
    dq8.ico          Windows, 16 to 256 px; dq8.rc puts it in an executable
    dq8.icns         macOS app bundles, 16 to 1024 px
    dq8-window.bmp   64 px, the window's own icon on Linux, whose executables
                     carry none

Each size is drawn on its own, so the small ones stay sharp. Standard library
only, so it runs wherever Python does:

    python3 res/icon/make_icon.py
"""
from __future__ import annotations

import math
import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent

# The launcher's palette (src/ui/ui_style.h) and backdrop (launcher_app.cpp).
NAVY_TOP = (24, 34, 92)
NAVY_BOTTOM = (8, 11, 32)
GLOW = (52, 74, 170)
BORDER = (242, 243, 252)
GOLD_TOP = (255, 228, 134)
GOLD_BOTTOM = (214, 158, 38)
JEWEL = (200, 40, 60)

# The crown as ui_glyphs.cpp draws Icon::Crown, in units of its size.
SPIKES = [((-0.4, 0.14), (-0.42, -0.26), (-0.1, 0.14)),
          ((-0.2, 0.14), (0.0, -0.36), (0.2, 0.14)),
          ((0.1, 0.14), (0.42, -0.26), (0.4, 0.14))]
BAND = ((-0.4, 0.08), (0.4, 0.3), 0.04)
TIPS = [((-0.42, -0.28), 0.065), ((0.0, -0.38), 0.065), ((0.42, -0.28), 0.065)]
GEM = ((0.0, 0.19), 0.05)
CROWN_TOP, CROWN_BOTTOM = -0.445, 0.3

# Night sky, as behind the launcher's window: (x, y, radius), in the window's units.
STARS = [(0.2, 0.2, 0.007), (0.31, 0.14, 0.004), (0.79, 0.19, 0.006), (0.7, 0.13, 0.0035),
         (0.86, 0.33, 0.004), (0.15, 0.36, 0.0035)]


def round_box(px: float, py: float, x0: float, y0: float, x1: float, y1: float, r: float) -> float:
    qx = abs(px - (x0 + x1) * 0.5) - (x1 - x0) * 0.5 + r
    qy = abs(py - (y0 + y1) * 0.5) - (y1 - y0) * 0.5 + r
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0) - r


def triangle(px: float, py: float, a: tuple, b: tuple, c: tuple) -> float:
    """Signed distance to a triangle (Inigo Quilez's)."""
    best, outside = float("inf"), False
    orient = math.copysign(1.0, (b[0] - a[0]) * (a[1] - c[1]) - (b[1] - a[1]) * (a[0] - c[0]))
    for (sx, sy), (ex, ey) in ((a, b), (b, c), (c, a)):
        dx, dy, vx, vy = ex - sx, ey - sy, px - sx, py - sy
        t = min(max((vx * dx + vy * dy) / (dx * dx + dy * dy), 0.0), 1.0)
        best = min(best, (vx - dx * t) ** 2 + (vy - dy * t) ** 2)
        outside = outside or orient * (vx * dy - vy * dx) < 0.0
    return math.sqrt(best) if outside else -math.sqrt(best)


def crown(gx: float, gy: float) -> float:
    d = round_box(gx, gy, *BAND[0], *BAND[1], BAND[2])
    for spike in SPIKES:
        d = min(d, triangle(gx, gy, *spike))
    for (cx, cy), r in TIPS:
        d = min(d, math.hypot(gx - cx, gy - cy) - r)
    return d


def coverage(distance_px: float) -> float:
    return min(max(0.5 - distance_px, 0.0), 1.0)


def over(dst: list, color: tuple, alpha: float) -> None:
    """Paints color at alpha over dst, an [r, g, b, a] in 0-1 floats."""
    if alpha <= 0.0:
        return
    out = alpha + dst[3] * (1.0 - alpha)
    for i in range(3):
        dst[i] = (color[i] / 255.0 * alpha + dst[i] * dst[3] * (1.0 - alpha)) / out
    dst[3] = out


def mix(a: tuple, b: tuple, t: float) -> tuple:
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def draw(size: int, margin: float) -> bytes:
    """RGBA rows of the icon at size px, the window inset by margin (0-1)."""
    n = float(size)
    small = size <= 24
    x0, x1 = margin, 1.0 - margin
    width = x1 - x0
    radius = width * 0.2
    # The window's white edge, inset from a dark rim, and its faint inner line.
    edge = max(1.0 / n, width * 0.028)
    edge_at = max(1.0 / n, width * 0.022) + edge * 0.5
    line = max(1.0 / n, width * 0.008)
    line_at = edge_at + edge * 0.5 + width * 0.035
    # The crown, a little larger where few pixels have to carry it.
    scale = width * (0.74 if small else 0.64)
    cx = 0.5
    cy = 0.5 + width * 0.03 - (CROWN_TOP + CROWN_BOTTOM) * 0.5 * scale
    crown_box = (cx - 0.5 * scale, cy + (CROWN_TOP - 0.05) * scale,
                 cx + 0.5 * scale, cy + (CROWN_BOTTOM + 0.08) * scale)
    shadow = width * 0.018
    rows = bytearray()
    for y in range(size):
        v = (y + 0.5) / n
        for x in range(size):
            u = (x + 0.5) / n
            pixel = [0.0, 0.0, 0.0, 0.0]
            window = round_box(u, v, x0, x0, x1, x1, radius)
            if window * n < 0.5:
                depth = (v - x0) / width
                sky = mix(NAVY_TOP, NAVY_BOTTOM, depth)
                glow = max(0.0, 1.0 - math.hypot(u - 0.5, (v - x0 - width * 0.18) * 1.4) / (width * 0.7))
                sky = mix(sky, GLOW, 0.4 * glow * glow)
                if depth > 0.7:
                    sky = mix(sky, GOLD_BOTTOM, 0.08 * (depth - 0.7) / 0.3)
                over(pixel, sky, coverage(window * n))
                if size >= 128:
                    for sx, sy, sr in STARS:
                        star = math.hypot(u - (x0 + sx * width), v - (x0 + sy * width)) - sr * width
                        over(pixel, (255, 255, 255), 0.85 * coverage(star * n))
                over(pixel, BORDER, coverage((abs(window + edge_at) - edge * 0.5) * n))
                if not small:
                    over(pixel, BORDER, 0.22 * coverage((abs(window + line_at) - line * 0.5) * n))
                if crown_box[0] <= u <= crown_box[2] and crown_box[1] <= v <= crown_box[3] + shadow:
                    # A soft shadow under the crown lifts it off the sky.
                    cast = crown((u - cx) / scale, (v - shadow - cy) / scale) * scale
                    over(pixel, (0, 0, 8), 0.45 * min(max(0.5 - cast / (width * 0.02), 0.0), 1.0))
                    gx, gy = (u - cx) / scale, (v - cy) / scale
                    shade = (gy - CROWN_TOP) / (CROWN_BOTTOM - CROWN_TOP)
                    over(pixel, mix(GOLD_TOP, GOLD_BOTTOM, shade), coverage(crown(gx, gy) * scale * n))
                    (jx, jy), jr = GEM
                    over(pixel, JEWEL, coverage((math.hypot(gx - jx, gy - jy) - jr) * scale * n))
                    if size >= 64:
                        glint = math.hypot(gx - jx + jr * 0.35, gy - jy + jr * 0.35) - jr * 0.3
                        over(pixel, (255, 255, 255), 0.6 * coverage(glint * scale * n))
            rows += bytes(round(min(max(c, 0.0), 1.0) * 255) for c in pixel)
    return bytes(rows)


def png(size: int, rgba: bytes) -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    stride = size * 4
    raw = b"".join(b"\x00" + rgba[row * stride:(row + 1) * stride] for row in range(size))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def ico(images: list) -> bytes:
    """PNG entries, which Windows reads since Vista."""
    entries, data = b"", b""
    offset = 6 + 16 * len(images)
    for size, blob in images:
        entries += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(blob), offset + len(data))
        data += blob
    return struct.pack("<HHH", 0, 1, len(images)) + entries + data


def icns(images: list) -> bytes:
    body = b"".join(kind + struct.pack(">I", len(blob) + 8) + blob for kind, blob in images)
    return b"icns" + struct.pack(">I", len(body) + 8) + body


def bmp(size: int, rgba: bytes) -> bytes:
    """32-bit with an alpha mask in a BITMAPV4HEADER, as SDL_SaveBMP writes it."""
    stride = size * 4
    pixels = bytearray()
    for row in reversed(range(size)):
        line = rgba[row * stride:(row + 1) * stride]
        for i in range(0, stride, 4):
            pixels += bytes((line[i + 2], line[i + 1], line[i], line[i + 3]))
    info = struct.pack("<IiiHHIIiiII", 108, size, size, 1, 32, 3, len(pixels), 2835, 2835, 0, 0)
    info += struct.pack("<IIIII", 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000, 0x73524742)
    info += bytes(36 + 12)  # endpoints and gamma, unused with sRGB
    return struct.pack("<2sIHHI", b"BM", 14 + len(info) + len(pixels), 0, 0, 14 + len(info)) + info + bytes(pixels)


def main() -> None:
    drawn: dict = {}

    def image(size: int, margin: float) -> bytes:
        if (size, margin) not in drawn:
            print(f"make_icon: {size} px", flush=True)
            drawn[(size, margin)] = draw(size, margin)
        return drawn[(size, margin)]

    # Windows shows icons edge to edge; macOS sets them in Apple's grid,
    # 824 px of 1024.
    tight, apple = 0.03, 100.0 / 1024.0
    (HERE / "dq8.png").write_bytes(png(1024, image(1024, tight)))
    (HERE / "dq8.ico").write_bytes(ico([(size, png(size, image(size, tight)))
                                        for size in (16, 20, 24, 32, 40, 48, 64, 128, 256)]))
    kinds = [(b"icp4", 16), (b"icp5", 32), (b"icp6", 64), (b"ic07", 128), (b"ic08", 256), (b"ic09", 512),
             (b"ic10", 1024), (b"ic11", 32), (b"ic12", 64), (b"ic13", 256), (b"ic14", 512)]
    (HERE / "dq8.icns").write_bytes(icns([(kind, png(size, image(size, apple))) for kind, size in kinds]))
    (HERE / "dq8-window.bmp").write_bytes(bmp(64, image(64, tight)))
    print("make_icon: done", flush=True)


if __name__ == "__main__":
    main()
