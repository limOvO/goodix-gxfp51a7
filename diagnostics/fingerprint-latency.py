#!/usr/bin/env python3
"""Measures press-to-decision latency from driver logs.

Parses the debug log of a verification run and reports, per press:

    capture sequence   the five-command exchange that exposes the ridge frame
    image reception    waiting for and decrypting the TLS record
    scoring            descriptor matching and the decision

All figures come from the driver's own timestamps (`GX_...` state entries and
`fp_dbg` timing lines), so they measure the real path rather than estimate it.

Run a session with full debug first:

    sudo systemctl stop fprintd
    sudo env GX_TEMPLATE=/tmp/tmpl.bin \\
        LD_LIBRARY_PATH=/opt/libfprint-tod/lib \\
        FP_TOD_DRIVERS_DIR=/opt/libfprint-tod/lib/libfprint-2/tod-1 \\
        G_MESSAGES_DEBUG=all ./test-verify 5 > /tmp/timing.log 2>&1

    ./fingerprint-latency.py /tmp/timing.log
"""
import re
import statistics
import sys

TS = re.compile(r'DEBUG: (\d+:\d+:\d+\.\d+): (.*)')


def secs(stamp):
    h, m, s = stamp.split(':')
    return int(h) * 3600 + int(m) * 60 + float(s)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else '/tmp/timing.log'
    lines = []

    for raw in open(path, errors='replace'):
        m = TS.search(raw)
        if m:
            lines.append((m.group(1), m.group(2)))

    if not lines:
        sys.exit('no driver debug lines in %s (run with G_MESSAGES_DEBUG=all)' % path)

    rows = []

    # A press starts when the state machine enters the capture state, which the
    # driver only does after its own finger detection fires. That is the closest
    # observable point to "finger touched the sensor".
    for i, (t, msg) in enumerate(lines):
        if 'entering state 2' not in msg:
            continue

        seq = capt = dec = None
        score = None

        for t2, m2 in lines[i:i + 40]:
            if 'capture sequence' in m2:
                seq = secs(t2)
            if 'whole capture' in m2:
                capt = secs(t2)
            m3 = re.search(r'verify: attempt \d+ -> (\d+) matches', m2)
            if m3:
                dec = secs(t2)
                score = int(m3.group(1))
                break

        if dec is not None:
            rows.append((secs(t), seq, capt, dec, score))

    if not rows:
        sys.exit('no completed presses found')

    print('%-3s %11s %12s %11s %9s %7s'
          % ('#', 'sequence', 'image recv', 'scoring', 'TOTAL', 'score'))
    print('-' * 60)

    totals = []
    for n, (det, seq, capt, dec, score) in enumerate(rows, 1):
        total = (dec - det) * 1000
        totals.append(total)
        print('%-3d %10.0fms %11.0fms %10.0fms %8.0fms %7d'
              % (n, (seq - det) * 1000, (capt - seq) * 1000,
                 (dec - capt) * 1000, total, score))

    print()
    print('presses      : %d' % len(totals))
    print('median       : %.0f ms' % statistics.median(totals))
    print('min / max    : %.0f / %.0f ms' % (min(totals), max(totals)))

    avg_seq = statistics.mean([(r[1] - r[0]) * 1000 for r in rows])
    avg_img = statistics.mean([(r[2] - r[1]) * 1000 for r in rows])
    avg_sc = statistics.mean([(r[3] - r[2]) * 1000 for r in rows])

    print()
    print('average breakdown:')
    print('  capture sequence : %6.0f ms  (%.0f%%)'
          % (avg_seq, 100 * avg_seq / statistics.mean(totals)))
    print('  image reception  : %6.0f ms  (%.0f%%)'
          % (avg_img, 100 * avg_img / statistics.mean(totals)))
    print('  scoring          : %6.0f ms  (%.0f%%)'
          % (avg_sc, 100 * avg_sc / statistics.mean(totals)))
    print()
    # Detection granularity: the poll loop cannot see a press before its next
    # FDT probe, and each probe is itself a protocol exchange.
    polls = [secs(t) for t, m in lines if 'wait-on' in m]
    if len(polls) > 2:
        gaps = [(polls[i + 1] - polls[i]) * 1000 for i in range(len(polls) - 1)]
        gaps = [g for g in gaps if g < 1500]      # exclude idle gaps
        if gaps:
            print()
            print('detection polling: %.0f ms between probes (max %.0f ms)'
                  % (statistics.mean(gaps), max(gaps)))
            print('  so a press waits 0-%.0f ms (avg %.0f ms) just to be noticed'
                  % (max(gaps), statistics.mean(gaps) / 2))
            print('  -> typical touch-to-verdict: %.0f ms'
                  % (statistics.median(totals) + statistics.mean(gaps) / 2))

    print()
    print('The sequence dominates: it is five protocol commands, each paying a')
    print('drain wait. The gap and drain values are load-bearing (drain 2 wedges')
    print('the sensor outright), so this is close to the floor for this part.')


if __name__ == '__main__':
    main()
