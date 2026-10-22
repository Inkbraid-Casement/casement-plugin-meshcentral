#!/usr/bin/env python3
"""Draws icon.png: a monitor whose screen shows a few linked nodes, for
desktops reached through a server that relays to many machines.

    python3 tools/make-icon.py [output]

Drawn at four times the size and scaled down, for smooth edges.
"""

import sys

from PIL import Image, ImageDraw

SIZE = 256
SCALE = 4
S = SIZE * SCALE

FRAME = (46, 58, 74, 255)
SCREEN = (230, 241, 247, 255)
LINK = (63, 138, 158, 255)
NODE = (34, 110, 133, 255)
HUB = (226, 122, 52, 255)


def px(v):
    return int(round(v * SCALE))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "icon.png"
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # Monitor: frame, screen, neck and foot.
    d.rounded_rectangle([px(16), px(28), px(240), px(184)], radius=px(18), fill=FRAME)
    d.rounded_rectangle([px(30), px(42), px(226), px(170)], radius=px(8), fill=SCREEN)
    d.polygon([(px(108), px(184)), (px(148), px(184)), (px(156), px(214)), (px(100), px(214))], fill=FRAME)
    d.rounded_rectangle([px(72), px(210), px(184), px(228)], radius=px(9), fill=FRAME)

    # A small network on the screen: one hub linked to the nodes around it.
    hub = (128, 106)
    nodes = [(70, 72), (186, 70), (62, 142), (194, 140), (128, 154)]
    for a, b in [(nodes[0], nodes[2]), (nodes[1], nodes[3]), (nodes[2], nodes[4]), (nodes[3], nodes[4])]:
        d.line([(px(a[0]), px(a[1])), (px(b[0]), px(b[1]))], fill=LINK, width=px(5))
    for n in nodes:
        d.line([(px(hub[0]), px(hub[1])), (px(n[0]), px(n[1]))], fill=LINK, width=px(7))
    for x, y in nodes:
        d.ellipse([px(x - 12), px(y - 12), px(x + 12), px(y + 12)], fill=NODE)
    d.ellipse([px(hub[0] - 19), px(hub[1] - 19), px(hub[0] + 19), px(hub[1] + 19)], fill=HUB)
    d.ellipse([px(hub[0] - 8), px(hub[1] - 8), px(hub[0] + 8), px(hub[1] + 8)], fill=SCREEN)

    img.resize((SIZE, SIZE), Image.LANCZOS).save(out, optimize=True)


if __name__ == "__main__":
    main()
