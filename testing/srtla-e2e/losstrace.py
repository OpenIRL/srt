#!/usr/bin/env python3
"""Per-loss timelines from a heavy-logging receiver log (build with CMAKE_EXTRA=-DENABLE_HEAVY_LOGGING=ON,
run with RECEIVER_DEBUG=1). For every packet the receiver dropped it shows when the gap was detected,
every request, and how late the drop came; then it sorts the drops into classes:

  unrequested      dropped without a single request (hold / timer / detection too late)
  one-request      requested once, no answer within the budget, no second request
  repeats-too-late requested several times, the last answer could not make the deadline
  answered-late    a retransmission arrived but after the play time (dropped anyway)

    losstrace.py results/<out>/<scenario>/<variant>/rcv.err [--verbose]
"""
import re, sys, statistics
from collections import defaultdict

path = sys.argv[1]; verbose = '--verbose' in sys.argv
ts_re = re.compile(r'^(\d+):(\d+):(\d+)\.(\d+)')
def ts(line):
    m = ts_re.match(line)
    return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 1e6 if m else None

detect, requests, recovered, dropped, dropped_delay = {}, defaultdict(list), {}, {}, {}
first_ts = None
for line in open(path, errors='replace'):
    t = ts(line)
    if t is None: continue
    if first_ts is None: first_ts = t
    m = re.search(r'LOSSTRACE detect %(\d+)-%(\d+)', line)
    if m:
        for sq in range(int(m.group(1)), int(m.group(2)) + 1): detect.setdefault(sq, t)
        continue
    m = re.search(r'LOSSTRACE request (first|repeat) %(\d+)-%(\d+) age (\d+) ms remaining (-?\d+) ms hold (\d+) ms spacing (\d+) ms', line)
    if m:
        for sq in range(int(m.group(2)), int(m.group(3)) + 1): requests[sq].append((t, m.group(1), int(m.group(5))))
        continue
    m = re.search(r'LOSSTRACE recovered %(\d+) after (-?\d+) ms (retransmission|original)', line)
    if m:
        recovered.setdefault(int(m.group(1)), (t, int(m.group(2)), m.group(3))); continue
    # heavy-logging builds name the dropped range exactly: "DROPSEQ: up to seqno %X (N packets) ... delayed D ms"
    m = re.search(r'tsbpd: DROPSEQ: up to seqno %(\d+) \((\d+) packets\).*delayed (\d+)\.(\d+) ms', line)
    if m:
        last, n, d = int(m.group(1)), int(m.group(2)), float(m.group(3) + '.' + m.group(4))
        for sq in range(last - n + 1, last + 1): dropped[sq] = t; dropped_delay[sq] = d
        continue
    # plain builds only log the packet delivered after the gap; attribute the drop to the packets before it
    m = re.search(r'RCV-DROPPED (\d+) packet\(s\)\. Packet seqno %(\d+) delayed for ([\d.]+) ms', line)
    if m and 'DROPSEQ' not in line:
        n, nxt, d = int(m.group(1)), int(m.group(2)), float(m.group(3))
        for sq in range(nxt - n, nxt):
            if sq not in dropped: dropped[sq] = t; dropped_delay[sq] = d

classes = defaultdict(list)
for sq, td in dropped.items():
    reqs = requests.get(sq, []); t0 = detect.get(sq)
    if sq in recovered and recovered[sq][0] <= td: cls = 'answered-late'
    elif not reqs: cls = 'unrequested'
    elif len(reqs) == 1: cls = 'one-request'
    else: cls = 'repeats-too-late'
    classes[cls].append(sq)
    if verbose:
        rel = lambda x: '%.0f' % ((x - t0) * 1000) if t0 else '?'
        print('drop %%%d: detected t=%.1fs, requests at +%s ms, dropped at +%s ms (late by %.0f ms)%s' % (
            sq, (t0 or td) - first_ts, ', '.join(rel(r[0]) for r in reqs), rel(td), dropped_delay[sq],
            '' if sq not in recovered else ', recovered at +%s ms' % rel(recovered[sq][0])))

n = len(dropped)
print('%s: %d packets dropped, %d loss records detected, %d recovered (%d by retransmission)' % (
    path, n, len(detect), len(recovered), sum(1 for r in recovered.values() if r[2] == 'retransmission')))
for cls in ('unrequested', 'one-request', 'repeats-too-late', 'answered-late'):
    sqs = classes.get(cls, [])
    if not sqs: continue
    firsts = [ (requests[s][0][0] - detect[s]) * 1000 for s in sqs if s in detect and requests.get(s)]
    counts = [len(requests.get(s, [])) for s in sqs]
    print('  %-17s %4d (%3.0f %%)  requests per drop: median %d, max %d   first request after detection: median %s ms' % (
        cls, len(sqs), 100.0 * len(sqs) / n, statistics.median(counts) if counts else 0, max(counts) if counts else 0,
        '%.0f' % statistics.median(firsts) if firsts else '-'))
rec = [r[1] for r in recovered.values() if r[2] == 'retransmission' and r[1] >= 0]
if rec:
    rec.sort(); print('  recovered by retransmission: %d, time from detection p50 %d ms, p90 %d ms, p99 %d ms, max %d ms' % (
        len(rec), rec[len(rec)//2], rec[int(len(rec)*.9)], rec[int(len(rec)*.99)], rec[-1]))
req_counts = [len(v) for v in requests.values()]
if req_counts:
    print('  requests per detected record: mean %.2f, share with >1: %.0f %%' % (statistics.mean(req_counts), 100.0 * sum(1 for c in req_counts if c > 1) / len(req_counts)))
