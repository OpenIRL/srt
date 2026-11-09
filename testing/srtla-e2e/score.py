#!/usr/bin/env python3
"""Score a results directory: how well did the receiver do, and did a change make it better or worse?

    score.py OUT[,OUT2,...] --base BASE --head HEAD [--markdown FILE] [--json FILE] [--fail-on-regression]

Several results directories (runs with different --seed) are aggregated: sub-scores are averaged,
lost packets are summed.

Per scenario and variant, five sub-scores in 0..100 (higher is better):
  delivery   100 * exp(-lost permille / 5): what the application actually lost; 1 permille = 82, 5 = 37, 25 = 1
  efficiency 100 * (1 - duplicate share of retransmissions): a duplicate is a request the sender paid for twice
  bond       min share of a healthy link against half its fair share: does every usable link stay in the bond
  recovery   1 - (ACK-lag p99 / half the latency): how fast the oldest hole gets closed, relative to the budget
  latency    1 - (avoidable end-to-end latency / 500 ms): delivery later than the fastest link allows
Weights 0.40 / 0.20 / 0.15 / 0.15 / 0.10. A run whose host stalled (> 1.5 s gap) is excluded.
Total per variant = mean over scenarios. Verdict: regression if the total drops by more than 3, any
scenario by more than 10, the application lost 3+ packets where the base lost none, it lost more
than twice what the base lost (plus 5), or a receiver ran below the SRTLA minimum latency of 1000 ms.
"""
import argparse, json, math, os, sys
ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, ROOT)
from run_matrix import lag_percentiles
WEIGHTS = dict(delivery=0.40, efficiency=0.20, bond=0.15, recovery=0.15, latency=0.10)
STALL_LIMIT_MS = 1500

def clamp(x, lo=0.0, hi=1.0): return max(lo, min(hi, x))

def links_of(out, scn, variant):
    """(ips, scenario link configs) for a run, from its proxy.json."""
    try:
        cfg = json.load(open(os.path.join(ROOT, 'results', out, scn, variant, 'proxy.json')))
    except FileNotFoundError:
        return [], {}
    return list(cfg['links']), cfg['links']

def subscores(m, out, scn, variant):
    sent = max(1, m.get('snd_sent', 0) or m.get('app_rcvd', 0) + m.get('app_missing', 0))
    sc = {}
    sc['delivery'] = 100.0 * math.exp(-1000.0 * m.get('app_missing', 0) / sent / 5.0)   # 1 permille = 82, 5 = 37, 25 = 1
    rex = max(1, m.get('snd_retrans', 0))
    sc['efficiency'] = 100.0 * clamp(1.0 - m.get('rcv_belated', 0) / rex)
    ips, links = links_of(out, scn, variant)
    if ips:
        healthy = [ip for ip in ips if links[ip].get('up', True) and links[ip].get('delay_ms', 0) <= 500]
        if healthy:
            fair = 100.0 / len(ips)
            sc['bond'] = 100.0 * clamp(min(m['share'].get(ip, 0.0) for ip in healthy) / (0.5 * fair))
    lat = m.get('lat', m.get('lat_negotiated', 0)) or 0
    if m.get('lag_p99') is None:   # older summaries: derive from the trace file
        d = os.path.join(ROOT, 'results', out, scn, variant)
        m.update(lag_percentiles(d, max(1, m.get('snd_sent', 0)) / max(1, m.get('duration_s', 40))))
    if m.get('lag_p99') is not None and lat > 0:
        sc['recovery'] = 100.0 * clamp(1.0 - m['lag_p99'] / (0.5 * lat))
    if m.get('lat_p50_median', 0) > 0 and lat > 0 and ips:
        min_delay = min(links[ip].get('delay_ms', 0) for ip in ips)
        extra = m['lat_p50_median'] - lat - min_delay
        sc['latency'] = 100.0 * clamp(1.0 - max(0.0, extra) / 500.0)
    return sc

def total(sc):
    w = sum(WEIGHTS[k] for k in sc)
    return sum(WEIGHTS[k] * v for k, v in sc.items()) / w if w else 0.0

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out'); ap.add_argument('--base', required=True); ap.add_argument('--head', required=True)
    ap.add_argument('--markdown'); ap.add_argument('--json'); ap.add_argument('--fail-on-regression', action='store_true')
    ap.add_argument('--title', default='SRTLA end-to-end score')
    a = ap.parse_args()
    outs = a.out.split(',')
    rows, notes, excluded = [], [], []
    scns = []
    for out in outs:
        for scn in json.load(open(os.path.join(ROOT, 'results', out, 'summary.json'))):
            if scn not in scns: scns.append(scn)
    for scn in scns:
        agg = {}
        for out in outs:
            res = json.load(open(os.path.join(ROOT, 'results', out, 'summary.json')))
            for v in (a.base, a.head):
                m = res.get(scn, {}).get(v)
                if not m: continue
                if m.get('stall_max_ms', 0) > STALL_LIMIT_MS:
                    excluded.append('%s/%s/%s (host stall %d ms)' % (out, scn, v, m['stall_max_ms'])); continue
                if m.get('broken') or (m.get('lat_p50_median', 0) - (m.get('lat') or m.get('lat_negotiated') or 0) > 1000) or m.get('lag_p99') is None:
                    excluded.append('%s/%s/%s (stream died early, exit %s)' % (out, scn, v, m.get('exit', '?'))); continue
                sc = subscores(m, out, scn, v)
                e = agg.setdefault(v, dict(sub={}, n=0, missing=0, snddrop=0, nak=0, rexmit=0, belated=0, lat_rcv_min=10**9))
                e['lat_rcv_min'] = min(e['lat_rcv_min'], m.get('lat_rcv_effective', 10**9) if m.get('lat_rcv_effective', -1) > 0 else 10**9)
                for k, x in sc.items(): e['sub'][k] = e['sub'].get(k, 0.0) + x
                e['n'] += 1; e['missing'] += m.get('app_missing', 0); e['snddrop'] += m.get('snd_drop', 0)
                e['nak'] += m.get('rcv_nak', 0); e['rexmit'] += m.get('snd_retrans', 0); e['belated'] += m.get('rcv_belated', 0)
        pair = {}
        for v, e in agg.items():
            sub = {k: x / e['n'] for k, x in e['sub'].items()}
            pair[v] = dict(sub=sub, total=total(sub), runs=e['n'], missing=e['missing'], snddrop=e['snddrop'], nak=e['nak'], rexmit=e['rexmit'], belated=e['belated'], lat_rcv_min=e['lat_rcv_min'])
        if a.base in pair and a.head in pair:
            rows.append((scn, pair[a.base], pair[a.head]))
    if not rows:
        print('no comparable runs', file=sys.stderr); sys.exit(2)
    tb = sum(r[1]['total'] for r in rows) / len(rows); th = sum(r[2]['total'] for r in rows) / len(rows); delta = th - tb
    worst = min(rows, key=lambda r: r[2]['total'] - r[1]['total'])
    new_loss = [r[0] for r in rows if (r[2]['missing'] >= 3 and r[1]['missing'] == 0) or r[2]['missing'] > 2 * r[1]['missing'] + 5]
    violations = [r[0] for r in rows if r[2].get('lat_rcv_min', 1000) < 1000]   # SRTLA protocol minimum latency
    regression = delta < -3.0 or (worst[2]['total'] - worst[1]['total']) < -10.0 or bool(new_loss) or bool(violations)
    verdict = 'regression' if regression else 'improvement' if delta > 3.0 else 'neutral'
    icon = {'regression': ':red_circle:', 'improvement': ':green_circle:', 'neutral': ':white_circle:'}[verdict]

    md = ['### %s: %s **%.1f** (base %.1f, %+.1f) - %s' % (a.title, icon, th, tb, delta, verdict), '',
          '| scenario | base | head | delta | delivery | efficiency | bond | recovery | latency | missing | rexmit / dup |', '|---|---|---|---|---|---|---|---|---|---|---|']
    for scn, b, h in rows:
        d = h['total'] - b['total']
        cell = lambda k: ('%.0f' % h['sub'][k]) + ((' (%+.0f)' % (h['sub'][k] - b['sub'][k])) if k in b['sub'] and abs(h['sub'][k] - b['sub'][k]) >= 1 else '') if k in h['sub'] else '-'
        flag = ' :warning:' if d < -10 or scn in new_loss else ''
        md.append('| %s%s | %.1f | %.1f | %+.1f | %s | %s | %s | %s | %s | %d / %d | %d / %d |' % (
            scn, flag, b['total'], h['total'], d, cell('delivery'), cell('efficiency'), cell('bond'), cell('recovery'), cell('latency'),
            b['missing'], h['missing'], h['rexmit'], h['belated']))
    md += ['', 'Sub-scores are the head values, with the change against the base in brackets. %d scenario(s), %d run(s) each.' % (len(rows), rows[0][2]['runs'])]
    if new_loss: md.append('**Application-level loss beyond the base:** ' + ', '.join(new_loss))
    if violations: md.append('**Protocol violation, receiver latency below 1000 ms under SRTLA:** ' + ', '.join(violations))
    if excluded: md.append('Excluded (host stall during the run): ' + ', '.join(excluded))
    text = '\n'.join(md)
    print(text)
    if a.markdown: open(a.markdown, 'w').write(text + '\n')
    if a.json:
        json.dump(dict(base=a.base, head=a.head, total_base=tb, total_head=th, delta=delta, verdict=verdict, new_loss=new_loss, excluded=excluded,
                       scenarios={scn: dict(base=b, head=h) for scn, b, h in rows}), open(a.json, 'w'), indent=1)
    if a.fail_on_regression and regression:
        sys.exit(1)

if __name__ == '__main__':
    main()
