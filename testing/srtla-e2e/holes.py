#!/usr/bin/env python3
"""Recovery-time view from the sender's 20 ms send-buffer trace (snd.bstrace).

The unacknowledged send buffer (SRTO_SNDDATA) grows with the age of the oldest packet the
receiver has not yet acknowledged, i.e. with the time the oldest unrecovered loss has been
open. Dividing by the packet rate turns it into an "ACK lag" in milliseconds. This prints
the lag distribution per run, so first-NAK delay and repeat spacing become visible as time,
independent of latency and bitrate.

    holes.py OUT SCENARIO [VARIANTS]
"""
import json, os, statistics, sys
ROOT = os.path.dirname(os.path.abspath(__file__))
out, scn = sys.argv[1], sys.argv[2]
variants = sys.argv[3].split(',') if len(sys.argv) > 3 else sorted(os.listdir(os.path.join(ROOT, 'results', out, scn)))
print('%-8s%8s%8s%8s%8s%8s%9s%9s%9s   %s' % ('variant', 'p50 ms', 'p90 ms', 'p99 ms', 'max ms', 'mean', '>300ms', '>500ms', '>800ms', 'time share of ACK lag above threshold'))
for v in variants:
    d = os.path.join(ROOT, 'results', out, scn, v)
    try:
        rows = [tuple(map(float, l.split())) for l in open(os.path.join(d, 'snd.bstrace')) if l.strip()]
    except FileNotFoundError:
        continue
    snd = [json.loads(l) for l in open(os.path.join(d, 'snd.jsonl')) if l.strip() and 'final' not in l]
    pps = statistics.mean(r['pktSent'] for r in snd[2:]) if len(snd) > 2 else 1
    lag = sorted(bs / pps * 1000.0 for t, bs, fl, rx in rows if t > 3.0)   # skip start-up
    if not lag: continue
    q = lambda p: lag[min(len(lag) - 1, int(p * len(lag)))]
    share = lambda th: 100.0 * sum(1 for x in lag if x > th) / len(lag)
    print('%-8s%8.0f%8.0f%8.0f%8.0f%8.0f%8.1f%%%8.1f%%%8.1f%%' % (v, q(0.5), q(0.9), q(0.99), lag[-1], statistics.mean(lag), share(300), share(500), share(800)))
