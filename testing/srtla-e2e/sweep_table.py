#!/usr/bin/env python3
"""One row per variant over a results dir: total score against a reference variant plus the raw
numbers that matter for the hold/spacing trade-off.   sweep_table.py OUT REF_VARIANT"""
import json, os, statistics, sys
ROOT = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, ROOT)
from score import subscores, total
out, ref = sys.argv[1], sys.argv[2]
res = json.load(open(os.path.join(ROOT, 'results', out, 'summary.json')))
variants = sorted({v for s in res.values() for v in s}, key=lambda v: (v != ref, v))
print('%-10s%8s%8s%9s%9s%8s%9s%9s%10s%10s' % ('variant', 'score', 'vs ref', 'missing', 'rexmit', 'dup', 'lag p99', 'lag max', 'slow-link%', 'e2e-lat'))
totals = {}
for v in variants:
    tot, miss, rex, dup, lag99, lagmax, slow, e2e = [], 0, 0, 0, [], [], [], []
    for scn, s in res.items():
        m = s.get(v)
        if not m: continue
        tot.append(total(subscores(m, out, scn, v)))
        miss += m['app_missing']; rex += m['snd_retrans']; dup += m['rcv_belated']
        if m.get('lag_p99') is not None: lag99.append(m['lag_p99']); lagmax.append(m.get('lag_max', 0))
        ips = list(json.load(open(os.path.join(ROOT, 'results', out, scn, v, 'proxy.json')))['links'])
        if 'one_slow' in scn or 'two_slow' in scn: slow.append(m['share'].get(ips[-1], 0))
        e2e.append(m['lat_p50_median'] - m.get('lat', m.get('lat_negotiated', 0)))
    totals[v] = statistics.mean(tot) if tot else 0
    print('%-10s%8.1f%8s%9d%9d%8d%9.0f%9.0f%10s%10.0f' % (v, totals[v], ('%+.1f' % (totals[v] - totals[ref])) if v != ref else '-', miss, rex, dup,
          statistics.mean(lag99) if lag99 else 0, max(lagmax) if lagmax else 0, ('%.1f' % statistics.mean(slow)) if slow else '-', statistics.mean(e2e) if e2e else 0))
print('\nper scenario (total score):')
print('%-20s' % 'scenario' + ''.join('%10s' % v for v in variants))
for scn, s in res.items():
    print('%-20s' % scn + ''.join('%10.1f' % total(subscores(s[v], out, scn, v)) if v in s else '%10s' % '-' for v in variants))
