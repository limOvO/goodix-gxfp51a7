#!/usr/bin/env python3
"""Reports whether a set of captures forms one connected stitchable graph.

Why this exists: recording more presses does not automatically produce a bigger
stitch. Measured on a 15-press session, the trusted-pair graph was properly two
disjoint components of 7 and 6 views, and no amount of layout work can bridge
them — the presses in one group simply do not overlap the presses in the other.
The fix is procedural (press adjacent regions in sequence), and this script is
how you see that while there is still time to re-record.

Usage:
    ./coverage-report.py <dir> [--csv pairs.csv]

Reads pairs.csv produced by:
    stitch-captures --stats --csv pairs.csv --out x.png <captures>
"""
import argparse
import collections
import csv
import os
import sys

MIN_INLIERS = 10


def load_pairs(path):
    edges = []
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            a = os.path.basename(row['a']).replace('.bin', '')
            b = os.path.basename(row['b']).replace('.bin', '')
            try:
                inl = int(row['inliers'])
            except (KeyError, ValueError):
                continue
            if inl >= MIN_INLIERS:
                edges.append((a, b, inl))
    return edges


def components(nodes, edges):
    parent = {n: n for n in nodes}

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for a, b, _ in edges:
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb

    groups = collections.defaultdict(list)
    for n in nodes:
        groups[find(n)].append(n)
    return sorted(groups.values(), key=len, reverse=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir')
    ap.add_argument('--csv', default=None)
    a = ap.parse_args()

    csvpath = a.csv or os.path.join(a.dir, 'pairs.csv')
    if not os.path.exists(csvpath):
        sys.exit('no %s - run stitch-captures --stats --csv first' % csvpath)

    caps = sorted(f[:-4] for f in os.listdir(a.dir)
                  if f.startswith('p') and f.endswith('.bin'))
    edges = load_pairs(csvpath)
    groups = components(caps, edges)

    print('captures: %d, trusted pairs: %d' % (len(caps), len(edges)))
    print('components: %s' % [len(g) for g in groups])
    print()

    for i, g in enumerate(groups, 1):
        if len(g) < 2:
            continue
        print('group %d (%d views): %s' % (i, len(g), ' '.join(sorted(g))))

    biggest = len(groups[0]) if groups else 0
    print()
    if biggest == len(caps):
        print('RESULT: one connected group - stitching will use every view.')
        return 0
    print('RESULT: split into %d group(s); the largest holds %d of %d views.'
          % (len([g for g in groups if len(g) > 1]), biggest, len(caps)))
    print()
    print('More presses will not bridge these groups on their own. What does:')
    print('  press ADJACENT regions in sequence, so each press shares a band')
    print('  with the one before it. A good pattern is a short sweep - e.g.')
    print('  centre, then a little up, then back through centre, then a little')
    print('  left - rather than visiting unrelated parts of the fingertip.')
    print('  Rotating the fingertip in place also keeps overlap high.')
    return 1


if __name__ == '__main__':
    sys.exit(main())
