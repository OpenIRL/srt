#!/usr/bin/env python3
"""Aggregate a scenario over several results dirs (different seeds): repeats_table.py OUT1,OUT2,... SCN1,SCN2 [VARIANTS]"""
import json, os, statistics, sys
ROOT = os.path.dirname(os.path.abspath(__file__))
outs = sys.argv[1].split(','); scns = sys.argv[2].split(','); variants = sys.argv[3].split(',') if len(sys.argv) > 3 else None
for scn in scns:
    print('\n### %s  (%s)' % (scn, ', '.join(outs)))
    print('%-8s%10s%8s%8s%9s%9s%9s' % ('variant', 'bs mean', 'NAKs', 'rexmit', 'belated', 'missing', 'lag p99'))
    rows_by_v = {}
    for o in outs:
        s = json.load(open(os.path.join(ROOT, 'results', o, 'summary.json'))).get(scn, {})
        for v, m in s.items():
            if variants and v not in variants: continue
            rows_by_v.setdefault(v, []).append(dict(bs=m['bs_mean'], nak=m['rcv_nak'], rex=m['snd_retrans'], bel=m['rcv_belated'], miss=m['app_missing'], lag=m.get('lag_p99', 0)))
    for v, rows in rows_by_v.items():
        g = lambda k: statistics.mean(r[k] for r in rows)
        print('%-8s%10.0f%8.0f%8.0f%9.0f%9.1f%9.0f   per run bs: %s | lag p99: %s' % (
            v, g('bs'), g('nak'), g('rex'), g('bel'), g('miss'), g('lag'), ' '.join(str(r['bs']) for r in rows), ' '.join(str(r['lag']) for r in rows)))
