#!/usr/bin/env python3
"""Validates stitch-captures.c against known ground truth.

Takes one real capture, generates synthetic views of it at KNOWN rotations and
translations with overlap, then runs the stitcher and compares the transforms
it recovers with the ones that were applied. An algorithm that cannot solve
this cannot be trusted on unknown data.
"""
import math
import os
import struct
import subprocess
import sys

W, H = 132, 112
D = '/tmp/stitch-gt'


def load(path):
    raw = open(path, 'rb').read()
    return list(struct.unpack('<%dH' % (W * H), raw))


def save(path, vals):
    open(path, 'wb').write(struct.pack('<%dH' % len(vals), *vals))


def rotate_shift(src, deg, tx, ty):
    """Rotates about the centre then translates, sampling bilinearly.

    This is the inverse of what the stitcher must recover: it finds the
    transform that maps view A onto view B, so applying (deg,tx,ty) to make B
    means the stitcher should report (-deg,-tx,-ty)-ish when aligning A->B.
    """
    out = [0] * (W * H)
    a = math.radians(deg)
    ca, sa = math.cos(a), math.sin(a)
    cx, cy = (W - 1) / 2.0, (H - 1) / 2.0

    for y in range(H):
        for x in range(W):
            # inverse map: undo translate, undo rotate, sample
            dx = x - tx - cx
            dy = y - ty - cy
            sx = ca * dx + sa * dy + cx
            sy = -sa * dx + ca * dy + cy
            if 0 <= sx < W - 1 and 0 <= sy < H - 1:
                ix, iy = int(sx), int(sy)
                fx, fy = sx - ix, sy - iy
                v = (src[iy * W + ix] * (1 - fx) * (1 - fy) +
                     src[iy * W + ix + 1] * fx * (1 - fy) +
                     src[(iy + 1) * W + ix] * (1 - fx) * fy +
                     src[(iy + 1) * W + ix + 1] * fx * fy)
                out[y * W + x] = int(v)
    return out


def main():
    src_bin = sys.argv[1] if len(sys.argv) > 1 else '/tmp/p001.bin'
    os.makedirs(D, exist_ok=True)

    img = load(src_bin)
    bg = load(src_bin + '.bg')

    # Ground truth: view 0 is the reference; the others are known transforms
    # of it, chosen to overlap by roughly half the pad.
    cases = [
        (0.0, 0, 0),
        (0.0, 26, 0),      # pure translation
        (12.0, -18, 14),   # rotation + translation
        (-15.0, 10, -20),  # opposite rotation
    ]

    paths = []
    for i, (deg, tx, ty) in enumerate(cases):
        v = rotate_shift(img, deg, tx, ty)
        b = rotate_shift(bg, deg, tx, ty)
        p = os.path.join(D, 'v%02d.bin' % (i + 1))
        # zero out the invalid border so it does not look like signal
        v = [max(0, x) for x in v]
        b = [x if x > 0 else 1 for x in b]
        save(p, v)
        save(p + '.bg', b)
        paths.append(p)
        print('v%02d: rot %+.0f deg, shift %+d,%+d' % (i + 1, deg, tx, ty))

    print('\nrunning stitcher on ground-truth views:')
    r = subprocess.run(['/tmp/stitch-captures', '--out', D + '/gt.png'] + paths,
                       capture_output=True, text=True)
    print(r.stdout, end='')
    if r.stderr.strip():
        print(r.stderr, end='')

    # Check: did it find all pairs, and are the rotations close to expected?
    print('\nexpected relationships between view 1 and each other view:')
    for i, (deg, tx, ty) in enumerate(cases[1:], start=1):
        print('  v01 -> v%02d : %+.0f deg' % (i + 1, deg))
    print('\n(stitcher reports v[i] -> v[j] transforms above; a correct run')
    print(' should recover ~ -deg for the reverse direction, with inliers >= 10)')


if __name__ == '__main__':
    main()
