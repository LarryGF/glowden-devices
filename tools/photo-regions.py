#!/usr/bin/env python3
"""Make [[layout.photo.led]] regions for a driver file from a product photo.

Give one point per LED zone, placed on the lit area in cropped-photo pixels. Each zone gets
the part of the picture nearer to its point than to any other (below --top), which is enough:
with cutout = "vivid" only the lit pixels inside a region light up.

  tools/photo-regions.py photo.png --crop 716 198 1046 1761 --top 850 \\
      --point 1:265,1280 --point 2:660,1390 ... --preview preview.png

Prints the TOML to paste into the driver file. --preview renders each zone in a different
color over the cut-out photo, to check placement. Needs Pillow and numpy.
"""

import argparse
import colorsys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter


def clip(poly, a, b, c):
    """Keeps the part of a convex polygon where a*x + b*y <= c."""
    out = []
    for i, p in enumerate(poly):
        q = poly[(i + 1) % len(poly)]
        fp = a * p[0] + b * p[1] - c
        fq = a * q[0] + b * q[1] - c
        if fp <= 0:
            out.append(p)
        if fp * fq < 0:
            t = fp / (fp - fq)
            out.append((p[0] + t * (q[0] - p[0]), p[1] + t * (q[1] - p[1])))
    return out


def regions(points, w, h, top):
    cells = {}
    for z, (x, y) in points.items():
        poly = [(0, top), (w, top), (w, h), (0, h)]
        for z2, (x2, y2) in points.items():
            if z2 != z:
                poly = clip(poly, x2 - x, y2 - y, (x2 * x2 + y2 * y2 - x * x - y * y) / 2)
        cells[z] = poly
    return cells


def vivid(rgba):
    """Same rule as the app's "vivid" cutout (LightingViewer.svelte)."""
    a = rgba.astype(float) / 255
    mx = a[..., :3].max(-1)
    mn = a[..., :3].min(-1)
    sat = np.where(mx > 0, (mx - mn) / np.maximum(mx, 1e-6), 0)
    return np.clip((sat - 0.25) / 0.35, 0, 1) * np.clip((mx - 0.18) / 0.35, 0, 1) * a[..., 3]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--crop", type=int, nargs=4, metavar=("X", "Y", "W", "H"))
    ap.add_argument("--top", type=float, default=0, help="regions start at this y (skips other lit parts)")
    ap.add_argument("--point", action="append", required=True, metavar="ZONE:X,Y")
    ap.add_argument("--preview")
    args = ap.parse_args()

    im = Image.open(args.image).convert("RGBA")
    if args.crop:
        x, y, w, h = args.crop
        im = im.crop((x, y, x + w, y + h))
    else:
        print(f"# no --crop; alpha bounding box is {im.getchannel('A').getbbox()}")
    w, h = im.size
    points = {}
    for p in args.point:
        z, xy = p.split(":")
        px, py = xy.split(",")
        points[int(z)] = (float(px), float(py))
    cells = regions(points, w, h, args.top)

    for z in sorted(cells):
        d = "M" + " L".join(f"{x:.0f} {y:.0f}" for x, y in cells[z]) + " Z"
        print(f'\n[[layout.photo.led]]\nzone = {z}\npath = "{d}"')

    if args.preview:
        lit = vivid(np.asarray(im))
        area = Image.new("L", (w, h), 0)
        ImageDraw.Draw(area).polygon([(0, args.top), (w, args.top), (w, h), (0, h)], fill=255)
        lit *= np.asarray(area) / 255
        layer = Image.new("RGBA", (w, h))
        draw = ImageDraw.Draw(layer)
        for i, (z, poly) in enumerate(sorted(cells.items())):
            r, g, b = colorsys.hsv_to_rgb(i / len(cells), 1, 1)
            draw.polygon(poly, fill=(int(r * 255), int(g * 255), int(b * 255), 255))
        la = np.asarray(layer).astype(float)
        la[..., 3] *= lit
        base = np.asarray(im).astype(float)
        lum = base[..., :3] @ [0.3, 0.59, 0.11]
        base[..., :3] = np.where(lit[..., None] > 0, lum[..., None], base[..., :3])
        base[..., 3] *= 1 - lit
        out = Image.new("RGBA", (w, h), (28, 30, 36, 255))
        out.alpha_composite(Image.fromarray(base.astype(np.uint8), "RGBA"))
        colors = Image.fromarray(la.astype(np.uint8), "RGBA")
        out.alpha_composite(colors.filter(ImageFilter.GaussianBlur(8)))
        out.alpha_composite(colors)
        out.save(args.preview)
        print(f"\n# preview written to {args.preview}")


if __name__ == "__main__":
    main()
