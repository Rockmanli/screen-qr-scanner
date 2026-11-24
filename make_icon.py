#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成二维码扫描器图标 app.ico（多尺寸，ico 里自带 16~256px）"""

import io
import os
import random
import struct
from PIL import Image, ImageDraw

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "app.ico")
BG = (15, 20, 28, 255)
STROKE = (58, 73, 88, 255)
PAPER = (245, 248, 252, 255)
BLUE = (78, 161, 255, 255)

GRID = 11  # 11x11 的二维码风格图案，带三个定位角


def modules():
    rnd = random.Random(20261124)
    cells = [[0] * GRID for _ in range(GRID)]
    for i in range(GRID):
        for j in range(GRID):
            cells[i][j] = 1 if rnd.random() < 0.48 else 0

    # 三个定位角：3x3 实心框 + 中心 1x1，外加一圈空白分隔
    for r0, c0 in ((0, 0), (0, GRID - 3), (GRID - 3, 0)):
        for i in range(-1, 4):
            for j in range(-1, 4):
                rr, cc = r0 + i, c0 + j
                if 0 <= rr < GRID and 0 <= cc < GRID:
                    cells[rr][cc] = 0
        for i in range(3):
            for j in range(3):
                cells[r0 + i][c0 + j] = 1 if (i in (0, 2) or j in (0, 2)) else 0
        cells[r0 + 1][c0 + 1] = 1
    return cells


CELLS = modules()


def render(size):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    radius = max(2, int(size * 0.18))
    lw = max(1, size // 48)
    d.rounded_rectangle((0, 0, size - 1, size - 1), radius=radius,
                        fill=BG, outline=STROKE, width=lw)

    quiet = 1
    n = GRID + quiet * 2
    pad = max(1, int(size * 0.10))
    avail = size - pad * 2
    mod = max(1, avail // n)
    side = mod * n
    x0 = (size - side) // 2
    y0 = (size - side) // 2
    d.rounded_rectangle((x0, y0, x0 + side - 1, y0 + side - 1),
                        radius=max(1, mod), fill=PAPER)
    for r in range(GRID):
        for c in range(GRID):
            if CELLS[r][c]:
                x = x0 + (r + quiet) * mod
                y = y0 + (c + quiet) * mod
                d.rectangle((x, y, x + mod - 1, y + mod - 1), fill=BLUE)
    return img


def save_ico(path, sizes):
    """手工组装 ICO：每个尺寸单独渲染并放 PNG，保证小尺寸也清晰。"""
    renders = [(s, render(s)) for s in sizes]
    blobs = []
    for s, im in renders:
        buf = io.BytesIO()
        im.save(buf, format="PNG", optimize=True)
        blobs.append(buf.getvalue())
    header = struct.pack("<HHH", 0, 1, len(renders))
    offset = 6 + 16 * len(renders)
    entries, payload = [], b""
    for (s, _), blob in zip(renders, blobs):
        entries.append(struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32,
                                   len(blob), offset))
        payload += blob
        offset += len(blob)
    with open(path, "wb") as f:
        f.write(header + b"".join(entries) + payload)
    return renders[-1][1]


if __name__ == "__main__":
    sizes = [16, 24, 32, 48, 64, 128, 256]
    biggest = save_ico(OUT, sizes)
    biggest.save(os.path.join(os.path.dirname(OUT), "icon_preview.png"))
    print("生成:", OUT, os.path.getsize(OUT), "bytes, sizes:", sizes)
