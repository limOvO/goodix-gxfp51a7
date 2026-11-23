#!/usr/bin/env python3
"""Renders Goodix capture dumps to PNG and merges them into one image.

The driver writes, per capture:
    pNNN.bin      raw 12-bit frame, 132x112, little-endian uint16
    pNNN.bin.bg   the background frame taken for that same capture

What is shown is the *difference* (background - finger), which is the signal
the matcher works on: a finger darkens the capacitive pixels, so a stronger
press is a brighter picture. That is why a well-centred press looks full and an
edge press looks like a bright strip on one side.

Modes:
    contact   all captures side by side, labelled   (default)
    mosaic    aligned into one canvas, using the same registration the driver
              uses for enrolment guidance

Usage:
    ./render-captures.py [--dir /run/goodixtls/dump] [--mode contact|mosaic]
                         [--out captures.png] [--scale 3]
"""
import argparse
import glob
import os
import struct
import sys
import zlib

W, H = 132, 112


# --- minimal PNG writer (no PIL needed) --------------------------------

def write_png(path, width, height, rgb):
    """rgb: bytes-like of length width*height*3."""
    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = bytearray()
    for y in range(height):
        raw.append(0)                                   # filter: none
        raw += rgb[y * width * 3:(y + 1) * width * 3]

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)


# --- capture loading --------------------------------------------------

def load_u16(path):
    data = open(path, 'rb').read()
    if len(data) != W * H * 2:
        return None
    return struct.unpack('<%dH' % (W * H), data)


def load_capture(binpath):
    """Returns (name, diff list, stats) or None."""
    img = load_u16(binpath)
    bg = load_u16(binpath + '.bg')
    if img is None or bg is None:
        return None
    diff = [b - i for b, i in zip(bg, img)]
    mean = sum(diff) / len(diff)
    return os.path.basename(binpath), diff, mean


def shared_range(all_diffs, lo_pct=1.0, hi_pct=99.5):
    """One brightness scale for the whole set.

    Normalising each capture to its own full range is misleading: a nearly
    empty press gets stretched to look as strong as a good one. A single range
    keeps them comparable, which is the point of putting them side by side."""
    every = sorted(v for d in all_diffs for v in d)
    lo = every[int(len(every) * lo_pct / 100)]
    hi = every[int(len(every) * hi_pct / 100)]
    if hi <= lo:
        hi = lo + 1
    return lo, hi


def normalise(diff, rng):
    lo, hi = rng
    return [max(0, min(255, int(255.0 * (v - lo) / (hi - lo)))) for v in diff]


def to_grey(vals):
    """255 grey scale -> RGB triples."""
    out = bytearray(len(vals) * 3)
    for i, v in enumerate(vals):
        out[3 * i] = out[3 * i + 1] = out[3 * i + 2] = v
    return out


# --- 5x7 bitmap font, enough for labels -------------------------------

FONT = {
    'p': [0x0E, 0x11, 0x11, 0x1F, 0x10, 0x10, 0x10],
    '0': [0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E],
    '1': [0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E],
    '2': [0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F],
    '3': [0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E],
    '4': [0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02],
    '5': [0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E],
    '6': [0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E],
    '7': [0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08],
    '8': [0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E],
    '9': [0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C],
    '.': [0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C],
    '-': [0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00],
    ' ': [0] * 7,
}


def draw_text(rgb, cw, x0, y0, text, scale=1, colour=(255, 220, 120)):
    cx = x0
    for ch in text:
        glyph = FONT.get(ch, FONT[' '])
        for ry, row in enumerate(glyph):
            for rx in range(5):
                if not (row >> (4 - rx)) & 1:
                    continue
                for sy in range(scale):
                    for sx in range(scale):
                        px = cx + rx * scale + sx
                        py = y0 + ry * scale + sy
                        if 0 <= px < cw and 0 <= py < 10 ** 9:
                            o = (py * cw + px) * 3
                            if o + 2 < len(rgb):
                                rgb[o], rgb[o + 1], rgb[o + 2] = colour
        cx += 6 * scale


# --- modes -------------------------------------------------------------

def render_contact(caps, out, scale, rng, scores, pad=6, label_h=12):
    n = len(caps)
    cols = min(n, 4) or 1
    rows = (n + cols - 1) // cols
    tw, th = W * scale, H * scale
    cw = cols * (tw + pad) + pad
    ch = rows * (th + label_h + pad) + pad
    rgb = bytearray(cw * ch * 3)
    for i in range(cw * ch):
        rgb[3 * i:3 * i + 3] = b'\x10\x10\x14'          # dark background

    for i, (name, diff, mean) in enumerate(caps):
        r, c = divmod(i, cols)
        x0 = pad + c * (tw + pad)
        y0 = pad + r * (th + label_h + pad)
        vals = normalise(diff, rng)
        for y in range(H):
            for x in range(W):
                v = vals[y * W + x]
                for sy in range(scale):
                    py = y0 + y * scale + sy
                    base = (py * cw + x0) * 3
                    for sx in range(scale):
                        o = base + (x * scale + sx) * 3
                        if 0 <= o + 2 < len(rgb):
                            rgb[o] = rgb[o + 1] = rgb[o + 2] = v
        # label: capture index and mean signal
        draw_text(rgb, cw, x0 + 2, y0 + th + 2, '%02d' % (i + 1), 1)
        draw_text(rgb, cw, x0 + 20, y0 + th + 2, str(int(mean)), 1, (140, 200, 255))
        if i < len(scores) and scores[i] is not None:
            ok = scores[i] >= 18
            draw_text(rgb, cw, x0 + 60, y0 + th + 2, str(scores[i]), 1,
                      (120, 255, 140) if ok else (255, 120, 120))

    write_png(out, cw, ch, bytes(rgb))
    return out


def render_mosaic(caps, out, scale, rng, pad=8):
    """Aligns captures by their brightest region's centroid and averages.

    This is deliberately simple: the driver does proper SIFT registration for
    enrolment guidance, but a composite for the eye only needs the presses to
    land in roughly the right place, and a centroid shift is stable when the
    press covers most of the pad.
    """
    best = max(caps, key=lambda c: c[2]) if caps else None
    if not best:
        return None
    # canvas large enough for centroid spread
    acc = [0.0] * (W * H)
    cnt = [0] * (W * H)

    # reference centroid = the strongest capture's
    def centroid(diff):
        total = 0.0
        sx = sy = 0.0
        for y in range(H):
            for x in range(W):
                v = diff[y * W + x]
                if v > 0:
                    total += v
                    sx += v * x
                    sy += v * y
        if total == 0:
            return W / 2.0, H / 2.0
        return sx / total, sy / total

    rx, ry = centroid(best[1])
    for name, diff, mean in caps:
        cx, cy = centroid(diff)
        dx, dy = int(round(rx - cx)), int(round(ry - cy))
        for y in range(H):
            for x in range(W):
                tx, ty = x + dx, y + dy
                if 0 <= tx < W and 0 <= ty < H:
                    acc[ty * W + tx] += diff[y * W + x]
                    cnt[ty * W + tx] += 1

    merged = [acc[i] / cnt[i] if cnt[i] else 0.0 for i in range(W * H)]
    vals = normalise(merged, rng)

    tw, th = W * scale, H * scale
    cw, ch = tw + 2 * pad, th + 2 * pad
    rgb = bytearray(cw * ch * 3)
    for i in range(cw * ch):
        rgb[3 * i:3 * i + 3] = b'\x10\x10\x14'
    for y in range(H):
        for x in range(W):
            v = vals[y * W + x]
            for sy in range(scale):
                for sx in range(scale):
                    o = (((y * scale + sy) + pad) * cw + (x * scale + sx) + pad) * 3
                    rgb[o] = rgb[o + 1] = rgb[o + 2] = v
    write_png(out, cw, ch, bytes(rgb))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', default='/run/goodixtls/dump')
    ap.add_argument('--mode', default='contact', choices=('contact', 'mosaic', 'both'))
    ap.add_argument('--out', default=None)
    ap.add_argument('--scale', type=int, default=3)
    ap.add_argument('--scores', default='', help='comma-separated match scores')
    a = ap.parse_args()

    files = sorted(glob.glob(os.path.join(a.dir, 'p*.bin')))
    if not files:
        print('no captures in %s' % a.dir, file=sys.stderr)
        return 1

    caps = [c for c in (load_capture(f) for f in files) if c]
    if not caps:
        print('captures unreadable', file=sys.stderr)
        return 1

    print('%d capture(s):' % len(caps))
    for i, (name, diff, mean) in enumerate(caps):
        print('  %2d  %-28s mean=%6.1f' % (i + 1, name, mean))

    scores = [int(x) for x in a.scores.split(',') if x.strip()] if a.scores else []
    rng = shared_range([c[1] for c in caps])

    base = a.out or os.path.join(a.dir, 'captures')
    made = []
    if a.mode in ('contact', 'both'):
        made.append(render_contact(caps, base + '-contact.png', a.scale, rng, scores))
    if a.mode in ('mosaic', 'both'):
        m = render_mosaic(caps, base + '-mosaic.png', a.scale, rng)
        if m:
            made.append(m)
    for m in made:
        print('wrote %s (%d bytes)' % (m, os.path.getsize(m)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
