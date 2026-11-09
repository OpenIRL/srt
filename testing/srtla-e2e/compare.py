#!/usr/bin/env python3
"""Print one table per scenario from results/<out>/summary.json (default out: 'default')."""
import json, os, sys
ROOT = os.path.dirname(os.path.abspath(__file__))
out = sys.argv[1] if len(sys.argv) > 1 else 'default'
res = json.load(open(os.path.join(ROOT, 'results', out, 'summary.json')))
COLS = [('lat_negotiated', 'lat ms'), ('mbps_mean', 'Mbit/s'), ('app_missing', 'missing'), ('rcv_drop', 'rcvdrop'), ('snd_drop', 'snddrop'),
        ('rcv_nak', 'NAKs'), ('snd_retrans', 'rexmit'), ('rcv_belated', 'belated'), ('proxy_lost', 'injected'), ('bs_mean', 'bs mean'),
        ('lag_p99', 'lag p99'), ('lat_p50_median', 'e2e p50'), ('lat_p99_max', 'e2e p99'), ('stall_max_ms', 'stall')]
for name, vs in res.items():
    print('\n### %s' % name)
    print('%-8s' % 'variant' + ''.join('%10s' % c[1] for c in COLS) + '   share %')
    for v, m in vs.items():
        row = '%-8s' % v + ''.join('%10s' % m.get(c[0], '') for c in COLS)
        share = ' '.join('%s:%s' % (ip.split('.')[-1] if ip != '127.0.0.1' else 'lo', s) for ip, s in m['share'].items())
        print(row + '   ' + share)
