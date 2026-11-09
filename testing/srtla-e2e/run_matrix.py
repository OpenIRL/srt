#!/usr/bin/env python3
"""Run impairment scenarios against one or more receiver builds and summarise.

    run_matrix.py VARIANTS [SCENARIOS] [--seed N] [--out NAME] [--ips A,B,C]

VARIANTS  comma-separated receiver builds (build/receiver-<name>, see build.sh)
SCENARIOS comma-separated keys of SCENARIOS below (default: all)
Results land in results/<out>/<scenario>/<variant>/ plus results/<out>/summary.json.
Runs for one scenario are interleaved over the variants (base, head, base, head ...), so both see
the same host conditions.
"""
import argparse, json, os, re, signal, statistics, subprocess, sys, time

ROOT = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(ROOT, os.environ.get('BUILD_DIR', 'build'))
LAT = 2000          # SRT latency (ms) unless a scenario sets 'lat'
PORT_RCV, PORT_PROXY, PORT_SRT = 15000, 16000, 17000   # keep clear of test-srt (uses 5000+)

def L(delay, jitter=0, loss=0.0, loss_down=0.0, rate=0, queue=300):
    """One link: one-way delay/jitter (ms), uplink loss, downlink loss, rate cap (kbps), queue (ms)."""
    return dict(delay_ms=delay, jitter_ms=jitter, loss=loss, loss_down=loss_down, rate_kbps=rate, queue_ms=queue)

# Timeline events: t is seconds after the first packet; 'link' indexes 'links'.
SCENARIOS = {
    # --- two/three links (also runnable natively on macOS) ---
    'clean2':        dict(kbps=6000, dur=40, links=[L(20), L(40)], timeline=[]),
    'asym_rtt':      dict(kbps=6000, dur=40, links=[L(20, 0, 0.01), L(150, 20, 0.01)], timeline=[]),
    'loss_both':     dict(kbps=6000, dur=40, links=[L(30, 5, 0.03, 0.03), L(50, 5, 0.03, 0.03)], timeline=[]),
    'heavy_loss':    dict(kbps=4000, dur=45, links=[L(30, 5, 0.08), L(60, 5, 0.08)], timeline=[]),
    'outage':        dict(kbps=6000, dur=45, links=[L(20, 0, 0.01), L(40, 0, 0.01), L(60, 0, 0.01)],
                          timeline=[dict(t=10, link=2, set=dict(loss=1.0)), dict(t=18, link=2, set=dict(loss=0.01)),
                                    dict(t=26, link=0, set=dict(loss=1.0)), dict(t=32, link=0, set=dict(loss=0.01))]),
    'thin_link':     dict(kbps=8000, dur=40, links=[L(20), L(30), L(80, 5, 0.0, 0.0, 800, 250)], timeline=[]),
    'highrate':      dict(kbps=16000, dur=40, links=[L(20, 0, 0.005), L(35, 0, 0.005)], timeline=[]),
    'jitter_reorder':dict(kbps=6000, dur=40, links=[L(40, 60, 0.005), L(40, 60, 0.005)], timeline=[]),
    'three_clean':   dict(kbps=8000, dur=40, links=[L(20, 0, 0.005), L(40, 0, 0.005), L(80, 10, 0.005)], timeline=[]),
    'three_rev':     dict(kbps=8000, dur=40, links=[L(20, 0, 0.01, 0.05), L(40, 0, 0.01, 0.05), L(80, 10, 0.01, 0.05)], timeline=[]),
    'three_outage_rev': dict(kbps=8000, dur=45, links=[L(20, 0, 0.01, 0.05), L(40, 0, 0.01, 0.05), L(80, 10, 0.01, 0.05)],
                          timeline=[dict(t=10, link=0, set=dict(loss=1.0, loss_down=1.0)), dict(t=20, link=0, set=dict(loss=0.01, loss_down=0.05))]),
}
# --- real-world catalogue, generated from templates -------------------------------------------
# Every template takes the link list and returns a scenario, so each one can run with any number
# of links, any latency (SRTLA allows 1000 ms upwards; encoders default to 2000 or 3000) and either
# link set. Link roles: index 0 = fastest carrier, index -1 = slowest.
LATENCIES = (1000, 2000, 3000, 4000, 5000)

def links_n(n, slow=False):
    """n carriers with staggered one-way delays. Fast set: 25/40/55/70 ms (four decent carriers);
    slow set: 100/120/140/160 ms (far gateways, RTTs of 200-320 ms). 0.5 % loss, a little jitter."""
    base, step = (100, 20) if slow else (25, 15)
    return [L(base + step * i, 3 + min(5, i * 2), 0.005) for i in range(n)]
FOUR = links_n(4); FOUR_SLOW = links_n(4, slow=True)

def outages(link, period, dur, t0, t_end):
    """Periodic full outages of one link (handover, tunnel): down for dur every period seconds."""
    ev = []; t = t0
    while t + dur <= t_end:
        ev.append(dict(t=t, link=link, set=dict(up=False))); ev.append(dict(t=t + dur, link=link, set=dict(up=True))); t += period
    return ev

def with_(links, idx, **o):
    links = [dict(l) for l in links]; links[idx].update(o); return links

def dur_for(lat, base):
    """Longer runs at higher latency: the buffer takes latency to fill and drain."""
    return base + max(0, (lat - 2000) // 1000) * 3

TEMPLATES = {
    # four healthy modems
    'baseline':  lambda ls, lat: dict(kbps=8000, dur=dur_for(lat, 40), links=ls, timeline=[]),
    # one carrier with a far higher base RTT (bad carrier, distant gateway): must not be starved nor poison the hold
    'one_slow':  lambda ls, lat: dict(kbps=8000, dur=dur_for(lat, 40), links=with_(ls, -1, delay_ms=ls[-1]['delay_ms'] + 200, jitter_ms=20, loss=0.01), timeline=[]),
    'two_slow':  lambda ls, lat: dict(kbps=8000, dur=dur_for(lat, 40), links=with_(with_(ls, -2, delay_ms=ls[-2]['delay_ms'] + 160, jitter_ms=20), -1, delay_ms=ls[-1]['delay_ms'] + 240, jitter_ms=30), timeline=[]),
    # bufferbloat: one link's delay ramps +560 ms over 8 s, holds, drains back (FIFO)
    'bloat':     lambda ls, lat: dict(kbps=8000, dur=dur_for(lat, 60), links=ls, timeline=[dict(t=10, link=1, ramp=dict(delay_ms=ls[1]['delay_ms'] + 560), dur=8), dict(t=30, link=1, ramp=dict(delay_ms=ls[1]['delay_ms']), dur=8)]),
    # bursty (Gilbert-Elliott) loss on two links: ~10-packet bursts, a few per second
    'bursty':    lambda ls, lat: dict(kbps=8000, dur=dur_for(lat, 45), links=with_(with_(ls, 1, burst=dict(p_bad=0.004, p_good=0.1, loss_bad=0.6)), -1, burst=dict(p_bad=0.004, p_good=0.1, loss_bad=0.6)), timeline=[]),
    # one modem flaps: 4 s down / 6 s up (loose SIM, marginal coverage)
    'flapping':  lambda ls, lat: dict(kbps=8000, dur=dur_for(lat, 60), links=ls, timeline=outages(len(ls) // 2, 10, 4, 8, 58)),
    # handover / short tunnel: 3 s outage every 15 s on the fastest link
    'handover':  lambda ls, lat: dict(kbps=8000, dur=dur_for(lat, 60), links=ls, timeline=outages(0, 15, 3, 10, 58)),
    # a modem that boots late: the slowest link joins at t=15 (srtla_send re-registers it once the path opens)
    'join_late': lambda ls, lat: dict(kbps=8000, dur=dur_for(lat, 50), links=with_(ls, -1, up=False), timeline=[dict(t=15, link=len(ls) - 1, set=dict(up=True))]),
    # uplink fast but downlink +90 ms on every link (LTE uplink scheduling): the NAK/ACK path is the slow one
    'asym_dir':  lambda ls, lat: dict(kbps=8000, dur=dur_for(lat, 40), links=[dict(l, delay_down_ms=l['delay_ms'] + 90, loss=0.01) for l in ls], timeline=[]),
    # HARQ-style reordering inside every link (non-FIFO jitter)
    'reorder':   lambda ls, lat: dict(kbps=8000, dur=dur_for(lat, 40), links=[dict(l, jitter_ms=15, reorder=True) for l in ls], timeline=[]),
    # duplicated packets on one link
    'dup':       lambda ls, lat: dict(kbps=8000, dur=dur_for(lat, 40), links=with_(ls, 1, dup=0.02), timeline=[]),
    # capacity: every link rate-capped, the bond shrinks below the bitrate for 15 s (queues overflow;
    # the encoder-side controller that would back off is not part of this harness)
    'capacity':  lambda ls, lat: dict(kbps=2000, dur=dur_for(lat, 70), links=[dict(l, rate_kbps=3000, queue_ms=300) for l in ls],
                                      timeline=[dict(t=30, link=i, set=dict(rate_kbps=1600 // len(ls))) for i in range(len(ls))] + [dict(t=45, link=i, set=dict(rate_kbps=3000)) for i in range(len(ls))]),
    # dead zone: all links get 20 % loss and +300 ms for 15 s, then recover
    'dead_zone': lambda ls, lat: dict(kbps=2000, dur=dur_for(lat, 70), links=ls,
                                      timeline=[dict(t=30, link=i, set=dict(loss=0.2, delay_ms=ls[i]['delay_ms'] + 300)) for i in range(len(ls))] + [dict(t=45, link=i, set=dict(loss=0.005, delay_ms=ls[i]['delay_ms'])) for i in range(len(ls))]),
    'highrate':  lambda ls, lat: dict(kbps=25000, dur=dur_for(lat, 40), links=ls, timeline=[]),
    'lowrate':   lambda ls, lat: dict(kbps=400, dur=dur_for(lat, 45), links=with_(with_(ls, 0, loss=0.02), 1, loss=0.02), timeline=[]),
}

def make(name, links, lat):
    scn = TEMPLATES[name](links, lat); scn['lat'] = lat; return scn

SETS = {'rw': [], 'rw-lat': [], 'rw-slow': [], 'rw-links': [], 'rw-extra': []}
for _n in TEMPLATES:
    SCENARIOS['rw_' + _n] = make(_n, FOUR, 2000); SETS['rw'].append('rw_' + _n)                        # the reference catalogue
    for _lat in LATENCIES:
        if _lat != 2000:
            SCENARIOS['rw_%s_lat%d' % (_n, _lat)] = make(_n, FOUR, _lat); SETS['rw-lat'].append('rw_%s_lat%d' % (_n, _lat))
    SCENARIOS['rw_%s_slow' % _n] = make(_n, FOUR_SLOW, 2000); SETS['rw-slow'].append('rw_%s_slow' % _n)
    for _k in (2, 3, 5, 6, 7, 8):
        SCENARIOS['rw_%s_links%d' % (_n, _k)] = make(_n, links_n(_k), 2000); SETS['rw-links'].append('rw_%s_links%d' % (_n, _k))
# beyond the reorder budget: one link 1.1 s / 2 s late, a bloat ramp to 2 s, and the slow link listed
# first (srtla_send prepends its list, so the handshake takes the LAST listed link: compare the
# end-to-end latency with rw_one_slow to see whose arrival set the TSBPD time base)
SCENARIOS['rw_one_slow_1100'] = dict(make('baseline', with_(FOUR, -1, delay_ms=1100, jitter_ms=50), 2000), dur=45)
SCENARIOS['rw_one_slow_2000'] = dict(make('baseline', with_(FOUR, -1, delay_ms=2000, jitter_ms=50), 2000), dur=45)
SCENARIOS['rw_bloat_2000']    = dict(kbps=8000, dur=75, lat=2000, links=FOUR, timeline=[dict(t=10, link=1, ramp=dict(delay_ms=2000), dur=10), dict(t=40, link=1, ramp=dict(delay_ms=40), dur=10)])
SCENARIOS['rw_one_slow_first'] = make('baseline', with_(FOUR, 0, delay_ms=250, jitter_ms=20, loss=0.01), 2000)
SETS['rw-extra'] = ['rw_one_slow_1100', 'rw_one_slow_2000', 'rw_bloat_2000', 'rw_one_slow_first']
# protocol rule: SRTLA never runs below 1000 ms. Both sides ask for 500 ms; the receiver must raise
# to 1000 and negotiate that to the sender (score.py treats anything below 1000 as a violation).
SCENARIOS['rw_floor'] = make('baseline', FOUR, 500)
SCENARIOS['rw_soak'] = dict(kbps=8000, dur=300, lat=2000, links=with_(FOUR, -1, delay_ms=200, jitter_ms=20, burst=dict(p_bad=0.002, p_good=0.1, loss_bad=0.5)),
                            timeline=outages(2, 40, 4, 20, 290) + [dict(t=100, link=1, ramp=dict(delay_ms=500), dur=10), dict(t=130, link=1, ramp=dict(delay_ms=40), dur=10)])
# the pull-request set: the common configuration (four decent carriers, 2000 ms) plus the extremes
# of each dimension, on the whole catalogue: 5000 ms latency, 2 and 8 links, slow carriers. What
# holds at both ends is assumed to hold in between; the other latencies and link counts stay in
# 'rw-lat' / 'rw-links' / 'full' for manual runs.
SETS['ci'] = (SETS['rw'] + ['rw_%s_lat5000' % n for n in TEMPLATES] + ['rw_%s_links2' % n for n in TEMPLATES]
              + ['rw_%s_links8' % n for n in TEMPLATES] + ['rw_%s_slow' % n for n in TEMPLATES] + ['rw_floor'])
SETS['full'] = [k for k in SCENARIOS if k != 'rw_soak']

# --- latency sweep on two links (runs natively on macOS): 30/60 ms, 2 % loss up, 1 % down ---
# 500 ms is below the SRTLA minimum on purpose: it shows the raise to 1000 ms.
for _kbps in (6000, 500):
    for _lat in (500,) + LATENCIES:
        SCENARIOS['lat%d_%d' % (_lat, _kbps)] = dict(kbps=_kbps, dur=dur_for(_lat, 40), lat=_lat, links=[L(30, 5, 0.02, 0.01), L(60, 5, 0.02, 0.01)], timeline=[])
SETS['lat'] = [k for k in SCENARIOS if k.startswith('lat')]
# plain SRT control (no SRTLA at all): one link, the receiver is a normal SRT listener. The branch must
# not change plain SRT, so base and head have to come out identical here.
SCENARIOS['plain_srt'] = dict(kbps=6000, dur=40, lat=2000, plain=True, links=[L(30, 5, 0.02, 0.01)], timeline=[])
SETS['ci'].append('plain_srt')
# --- loss-focused set: the scenarios that separate algorithms (the clean ones any algorithm passes) ---
def all_links(ls, **o): return [dict(l, **o) for l in ls]
SCENARIOS.update({
    'loss_dead_zone':   SCENARIOS['rw_dead_zone'],                                                             # 20 % + 300 ms on all, 15 s
    'loss_dead_zone10': dict(kbps=2000, dur=70, lat=2000, links=FOUR,
                             timeline=[dict(t=30, link=i, set=dict(loss=0.10, delay_ms=FOUR[i]['delay_ms'] + 150)) for i in range(4)] + [dict(t=45, link=i, set=dict(loss=0.005, delay_ms=FOUR[i]['delay_ms'])) for i in range(4)]),
    'loss_all8':        dict(kbps=4000, dur=45, lat=2000, links=all_links(FOUR, loss=0.08), timeline=[]),          # 8 % everywhere, sustained
    'loss_two15':       dict(kbps=6000, dur=45, lat=2000, links=with_(with_(FOUR, 1, loss=0.15), 3, loss=0.15), timeline=[]),
    'loss_bursty_all':  dict(kbps=6000, dur=45, lat=2000, links=all_links(FOUR, burst=dict(p_bad=0.006, p_good=0.08, loss_bad=0.7)), timeline=[]),
    'loss_handover':    dict(kbps=6000, dur=60, lat=2000, links=all_links(FOUR, loss=0.03), timeline=outages(0, 15, 3, 10, 58)),
    'loss_capacity':    SCENARIOS['rw_capacity'],
    'loss_one_slow':    dict(kbps=6000, dur=45, lat=2000, links=with_(all_links(FOUR, loss=0.03), -1, delay_ms=250, jitter_ms=20, loss=0.05), timeline=[]),
    'loss_lat1000':     dict(kbps=6000, dur=45, lat=1000, links=all_links(FOUR, loss=0.04), timeline=[]),          # the floor: least budget
})
SETS['loss'] = [k for k in SCENARIOS if k.startswith('loss_')]
SETS['full'] = [k for k in SCENARIOS if k != 'rw_soak']

def local_ips():
    """Usable srtla_send bind addresses. Linux routes all of 127.0.0.0/8 on lo, so eight
    loopback addresses are always there; elsewhere every IPv4 of the host except link-local."""
    if sys.platform.startswith('linux'):
        return ['127.0.0.%d' % i for i in range(1, 9)]
    try:
        out = subprocess.run(['ifconfig'], capture_output=True, text=True).stdout
    except FileNotFoundError:
        out = subprocess.run(['ip', '-4', '-o', 'addr'], capture_output=True, text=True).stdout
    ips = []
    for ip in re.findall(r'inet (?:addr:)?(\d+\.\d+\.\d+\.\d+)', out):
        if ip.startswith('169.254.') or ip in ips:
            continue
        ips.append(ip)
    return ['127.0.0.1'] + [ip for ip in ips if ip != '127.0.0.1']

def run(scn, name, variant, out, seed, ips):
    d = os.path.join(out, name, variant); os.makedirs(d, exist_ok=True)
    if len(ips) < len(scn['links']):
        sys.exit('scenario %s needs %d links but only %d local IPs are usable: %s' % (name, len(scn['links']), len(ips), ips))
    ips = ips[:len(scn['links'])]
    open(os.path.join(d, 'ips.txt'), 'w').write('\n'.join(ips) + '\n')
    cfg = dict(listen=['0.0.0.0', PORT_PROXY], upstream=['127.0.0.1', PORT_RCV], seed=seed, log=os.path.join(d, 'proxy.jsonl'),
               links={ip: lc for ip, lc in zip(ips, scn['links'])},
               timeline=[dict(e, link=ips[e['link']]) for e in scn['timeline']])   # link index -> bind IP
    json.dump(cfg, open(os.path.join(d, 'proxy.json'), 'w'), indent=1)
    dur, lat = scn['dur'], scn.get('lat', LAT)
    procs = []
    def start(args, err):
        p = subprocess.Popen(args, stderr=open(os.path.join(d, err), 'w'), stdout=subprocess.DEVNULL); procs.append(p); return p
    plain = scn.get('plain', False)
    rcv = start([os.path.join(BUILD, 'receiver-' + variant), str(PORT_RCV), '--latency', str(lat), '--lossmaxttl', '50', '--kbps', str(scn['kbps']),
                 '--srtla', '0' if plain else '1', '--duration', str(dur + 25), '--stats', os.path.join(d, 'rcv.jsonl')]
                + (['--debug', '1'] if os.environ.get('RECEIVER_DEBUG') == '1' else []), 'rcv.err')
    prx = start([sys.executable, os.path.join(ROOT, 'proxy.py'), os.path.join(d, 'proxy.json')], 'proxy.err')
    time.sleep(0.5)
    if plain:
        ss = None; open(os.path.join(d, 'srtla_send.err'), 'w').write('plain SRT: no srtla_send\n')
    else:
        ss = start([os.path.join(BUILD, 'srtla_send'), str(PORT_SRT), '127.0.0.1', str(PORT_PROXY), os.path.join(d, 'ips.txt')], 'srtla_send.err')
    time.sleep(1.5)
    snd = start([os.path.join(BUILD, 'sender'), '127.0.0.1', str(PORT_PROXY if plain else PORT_SRT), '--bitrate', str(scn['kbps']), '--duration', str(dur),
                 '--latency', str(lat), '--stats', os.path.join(d, 'snd.jsonl'), '--bstrace', os.path.join(d, 'snd.bstrace')], 'snd.err')
    try:
        snd.wait(timeout=dur + 30 + lat // 1000)
    except subprocess.TimeoutExpired:
        print('  sender timeout', file=sys.stderr)
    time.sleep(1.0)
    for p in (ss, prx):
        if p is not None: p.send_signal(signal.SIGTERM)
    try: rcv.wait(timeout=10)
    except subprocess.TimeoutExpired: rcv.kill()
    for p in procs:
        try: p.wait(timeout=3)
        except subprocess.TimeoutExpired: p.kill()
    time.sleep(1.0)  # let the ports drain before the next run
    json.dump(dict(receiver=rcv.returncode, proxy=prx.returncode, srtla_send=(ss.returncode if ss else None), sender=snd.returncode),
              open(os.path.join(d, 'exit.json'), 'w'))
    return d

def jl(path):
    try: return [json.loads(l) for l in open(path) if l.strip()]
    except FileNotFoundError: return []

def lag_percentiles(d, pps):
    """ACK lag (age of the oldest unacknowledged packet) from the sender's 20 ms buffer trace, in ms."""
    try:
        rows = [tuple(map(float, l.split())) for l in open(os.path.join(d, 'snd.bstrace')) if l.strip()]
    except FileNotFoundError:
        return {}
    lag = sorted(bs / pps * 1000.0 for t, bs, fl, rx in rows if t > 3.0 and pps > 0)
    if not lag: return {}
    q = lambda p: round(lag[min(len(lag) - 1, int(p * len(lag)))])
    return dict(lag_p50=q(0.5), lag_p90=q(0.9), lag_p99=q(0.99), lag_max=round(lag[-1]))

def summarise(d, scn=None):
    snd = jl(os.path.join(d, 'snd.jsonl')); rcv = jl(os.path.join(d, 'rcv.jsonl')); prx = jl(os.path.join(d, 'proxy.jsonl'))
    s_rows = [r for r in snd if 'final' not in r]; r_rows = [r for r in rcv if 'final' not in r]
    r_fin = next((r for r in rcv if 'final' in r), {})
    mean = lambda xs: statistics.mean(xs) if xs else 0
    m = dict(
        snd_sent=sum(r['pktSent'] for r in s_rows), snd_retrans=sum(r['pktRetrans'] for r in s_rows), snd_nak=sum(r['pktRecvNAK'] for r in s_rows),
        snd_drop=sum(r['pktSndDrop'] for r in s_rows), app_drop=sum(r['app_drop'] for r in s_rows),
        bs_mean=round(mean([r['bs'] for r in s_rows])), bs_max=max((r['bs'] for r in s_rows), default=0),
        rtt_max=max((r['msRTT'] for r in s_rows), default=0), rtt_mean=round(mean([r['msRTT'] for r in s_rows]), 1),
        app_rcvd=r_fin.get('app_rcvd', 0), app_missing=r_fin.get('app_missing', 0), rcv_drop=sum(r['pktRcvDrop'] for r in r_rows),
        rcv_nak=sum(r['pktSentNAK'] for r in r_rows), rcv_retrans=sum(r['pktRcvRetrans'] for r in r_rows),
        rcv_belated=sum(r['pktRcvBelated'] for r in r_rows), rcv_loss_confirmed=sum(max(r['pktRcvLossConfirmed'], 0) for r in r_rows),
        lat_p99_max=max((r['lat_p99'] for r in r_rows), default=0), lat_max=r_fin.get('lat_max_all', 0),
        lat_p50_median=statistics.median([r['lat_p50'] for r in r_rows if r['lat_p50'] > 0]) if any(r['lat_p50'] > 0 for r in r_rows) else 0,
        mbps_mean=round(mean([r['mbpsRecvRate'] for r in r_rows]), 2),
    )
    for fn, key, pat in (('snd.err', 'lat_negotiated', r'Negotiated latency: (\d+)'), ('rcv.err', 'lat_rcv_effective', r'effective rcv latency (\d+)')):
        try:
            mm = re.search(pat, open(os.path.join(d, fn)).read()); m[key] = int(mm.group(1)) if mm else -1
        except FileNotFoundError:
            m[key] = -1
    # per link: uplink packets forwarded (= share) and packets the proxy took away (the injected loss)
    up, lost, ticks = {}, 0, []
    for r in prx:
        if 'link' not in r: continue
        up[r['link']] = up.get(r['link'], 0) + r['up_fwd']
        lost += r['up_loss'] + r.get('up_burst', 0) + r.get('up_dead', 0) + r['up_qdrop']
        if r['link'] == min(up): ticks.append(r['t'])
    tot = sum(up.values()) or 1
    m['share'] = {ip: round(100 * n / tot, 1) for ip, n in up.items()}
    m['proxy_lost'] = lost
    # a host stall shows as a late proxy tick (they are 1.0 s apart); such a run is not a receiver result
    m['stall_max_ms'] = round(max((b - a for a, b in zip(ticks, ticks[1:])), default=1.0) * 1000 - 1000)
    # a run is only a measurement if the stream lived for the whole duration: a sender that died
    # early (crash, connection break) leaves too few stat rows and an absurd end-to-end latency
    try: ex = json.load(open(os.path.join(d, 'exit.json')))
    except FileNotFoundError: ex = {}
    m['exit'] = ex
    m['broken'] = (scn is not None and len(s_rows) < scn['dur'] - 5) or (ex.get('sender') not in (None, 0)) or (ex.get('receiver') not in (None, 0))
    pps = m['snd_sent'] / max(1, len(s_rows))
    m.update(lag_percentiles(d, pps))
    if scn is not None:
        m['kbps'] = scn['kbps']; m['lat'] = scn.get('lat', LAT)
        m['min_delay_ms'] = min(l['delay_ms'] for l in scn['links'])
        m['healthy_links'] = sum(1 for l in scn['links'] if l.get('up', True) and l['delay_ms'] <= 500)
    return m

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('variants'); ap.add_argument('scenarios', nargs='?', default='all', help="comma list, or a set: rw (catalogue, 4 links, 2000 ms), rw-lat, rw-slow, rw-links, rw-extra, loss (lossy scenarios), ci (pull-request set), full, all")
    ap.add_argument('--group', help='k/n: run only every n-th scenario of the set, starting at k (parallel CI jobs)')
    ap.add_argument('--seed', type=int, default=1, help='proxy RNG seed (loss pattern)')
    ap.add_argument('--out', default='default', help='results/<out>/')
    ap.add_argument('--ips', help='comma-separated local bind IPs for srtla_send (default: auto-detect)')
    a = ap.parse_args()
    ips = a.ips.split(',') if a.ips else local_ips()
    out = os.path.join(ROOT, 'results', a.out); os.makedirs(out, exist_ok=True)
    summary_path = os.path.join(out, 'summary.json')
    results = json.load(open(summary_path)) if os.path.exists(summary_path) else {}
    print('links available:', ips, file=sys.stderr)
    names = []
    for tok in a.scenarios.split(','):      # each token is a set name or a scenario name
        for k in (SETS[tok] if tok in SETS else [k for k in SCENARIOS if k != 'rw_soak'] if tok == 'all' else [tok]):
            if k not in names: names.append(k)
    if a.group:
        k, n = map(int, a.group.split('/')); names = names[k - 1::n]   # deterministic split for parallel CI jobs
    for name in names:
        scn = SCENARIOS[name]
        for v in a.variants.split(','):
            t0 = time.time(); print('== %s / %s' % (name, v), file=sys.stderr, flush=True)
            d = run(scn, name, v, out, a.seed, ips)
            m = summarise(d, scn); results.setdefault(name, {})[v] = m
            print('   %.0fs  missing %d  rcvdrop %d  snddrop %d/%d  nak %d  retrans %d  belated %d  lag p99 %s  stall %d  share %s%s' % (
                time.time() - t0, m['app_missing'], m['rcv_drop'], m['snd_drop'], m['app_drop'], m['rcv_nak'], m['snd_retrans'], m['rcv_belated'],
                m.get('lag_p99', '-'), m['stall_max_ms'], m['share'], '  BROKEN RUN exit=%s' % m['exit'] if m['broken'] else ''), file=sys.stderr, flush=True)
            json.dump(results, open(summary_path, 'w'), indent=1)
