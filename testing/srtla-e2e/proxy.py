#!/usr/bin/env python3
"""Userland UDP link impairment proxy for SRTLA tests (no root needed).

srtla_send binds one socket per local source IP and sends every link to one SRTLA
host:port. This proxy listens there, identifies each link by its source IP, forwards it to
the real receiver from a per-link socket (so the receiver sees distinct link addresses) and
impairs each direction independently.

Per-link config keys (all optional):
  delay_ms, delay_down_ms      one-way delay per direction (down defaults to up)
  jitter_ms, jitter_down_ms    uniform extra delay per packet
  reorder                      false (default): jitter never reorders within the link (queueing)
                               true: packets may overtake each other (HARQ-style reordering)
  loss, loss_down              random loss per direction
  burst: {p_bad, p_good, loss_bad}   Gilbert-Elliott burst loss on the uplink, on top of 'loss'
  dup                          probability of duplicating an uplink packet
  rate_kbps, queue_ms          uplink capacity cap with a bounded FIFO queue (tail drop)
  up                           false = link dead in both directions (modem off, tunnel)

Timeline events (t = seconds after the first packet from srtla_send):
  {"t": 10, "link": "<ip>", "set":  {...}}                 apply immediately
  {"t": 10, "link": "<ip>", "ramp": {"delay_ms": 600}, "dur": 8}   linear ramp of numeric keys

Log: one JSON line per link and second with forwarded / lost / queue-dropped counters.
"""
import asyncio, json, random, socket, sys

NUMERIC = ('delay_ms', 'delay_down_ms', 'jitter_ms', 'jitter_down_ms', 'loss', 'loss_down', 'dup', 'rate_kbps', 'queue_ms')

class Link:
    def __init__(self, ip, cfg, seed, loop, upstream, log):
        self.ip = ip; self.cfg = dict(cfg); self.loop = loop; self.upstream = upstream; self.log = log
        self.rng = random.Random(seed)
        self.socks = {}                      # srtla_send socket addr -> upstream socket
        self.last_up = 0.0; self.last_down = 0.0
        self.bad = False                     # Gilbert-Elliott state
        self.backlog_ms = 0.0
        self.c = dict(up_fwd=0, up_bytes=0, up_loss=0, up_burst=0, up_qdrop=0, up_dup=0, up_dead=0, up_enobufs=0,
                      down_fwd=0, down_bytes=0, down_loss=0, down_dead=0, down_enobufs=0)
    # --- config ---
    def set(self, kv):
        self.cfg.update(kv)
    def g(self, key, default=0.0):
        return self.cfg.get(key, default)
    def delay(self, down):
        if down:
            d = self.g('delay_down_ms', self.g('delay_ms')) / 1000.0; j = self.g('jitter_down_ms', self.g('jitter_ms')) / 1000.0
        else:
            d = self.g('delay_ms') / 1000.0; j = self.g('jitter_ms') / 1000.0
        return d + (self.rng.random() * j if j > 0 else 0.0)
    def lossy(self):
        """Uplink loss decision: random loss plus an optional Gilbert-Elliott burst process."""
        b = self.cfg.get('burst')
        if b:
            if self.bad:
                if self.rng.random() < b.get('p_good', 0.1): self.bad = False
            elif self.rng.random() < b.get('p_bad', 0.0): self.bad = True
            if self.bad and self.rng.random() < b.get('loss_bad', 0.5):
                self.c['up_burst'] += 1; return True
        if self.rng.random() < self.g('loss'):
            self.c['up_loss'] += 1; return True
        return False
    # --- sender -> receiver ---
    def up(self, data, src, listen_sock):
        if src not in self.socks:
            us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); us.setblocking(False)
            us.bind(('127.0.0.1', 0)); us.connect(self.upstream)
            us.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
            self.socks[src] = us
            self.loop.add_reader(us, self.down, us, src, listen_sock)
        us = self.socks[src]
        if not self.cfg.get('up', True):
            self.c['up_dead'] += 1; return
        if self.lossy():
            return
        now = self.loop.time()
        depart = now + self.delay(False)
        if not self.cfg.get('reorder', False):
            depart = max(depart, self.last_up)                 # FIFO: queueing, no overtaking
        rate = self.g('rate_kbps') * 1000.0
        if rate > 0:
            depart = max(depart, self.last_up + len(data) * 8.0 / rate)
            q = depart - now - self.g('delay_ms') / 1000.0
            self.backlog_ms = q * 1000.0
            if q * 1000.0 > self.g('queue_ms', 300):
                self.c['up_qdrop'] += 1; return
        self.last_up = max(self.last_up, depart)
        self.loop.call_at(depart, self._send_up, us, data)
        if self.g('dup') > 0 and self.rng.random() < self.g('dup'):
            self.c['up_dup'] += 1
            self.loop.call_at(depart + 0.001, self._send_up, us, data)
    def _send_up(self, us, data):
        try:
            us.send(data); self.c['up_fwd'] += 1; self.c['up_bytes'] += len(data)
        except OSError:
            self.c['up_enobufs'] += 1
    # --- receiver -> sender ---
    def down(self, us, src, listen_sock):
        try:
            while True:
                data = us.recv(2048)
                if not self.cfg.get('up', True):
                    self.c['down_dead'] += 1; continue
                if self.rng.random() < self.g('loss_down'):
                    self.c['down_loss'] += 1; continue
                now = self.loop.time()
                depart = now + self.delay(True)
                if not self.cfg.get('reorder', False):
                    depart = max(depart, self.last_down)
                self.last_down = max(self.last_down, depart)
                self.loop.call_at(depart, self._send_down, listen_sock, data, src)
        except (BlockingIOError, InterruptedError):
            pass
        except OSError:
            pass
    def _send_down(self, listen_sock, data, src):
        try:
            listen_sock.sendto(data, src); self.c['down_fwd'] += 1; self.c['down_bytes'] += len(data)
        except OSError:
            self.c['down_enobufs'] += 1
    # --- log ---
    def flush(self, t):
        snap = dict(up=self.cfg.get('up', True), delay_ms=round(self.g('delay_ms'), 1), delay_down_ms=round(self.g('delay_down_ms', self.g('delay_ms')), 1),
                    loss=round(self.g('loss'), 4), loss_down=round(self.g('loss_down'), 4), rate_kbps=round(self.g('rate_kbps')), bad=self.bad)
        self.log.write(json.dumps(dict(t=round(t, 1), link=self.ip, backlog_ms=round(self.backlog_ms, 1), **self.c, cfg=snap)) + '\n'); self.log.flush()
        for k in self.c: self.c[k] = 0

async def main(cfg):
    loop = asyncio.get_running_loop()
    upstream = tuple(cfg['upstream'])
    log = open(cfg.get('log', 'proxy.jsonl'), 'w')
    ls = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); ls.setblocking(False)
    ls.bind(tuple(cfg['listen'])); ls.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    seed = cfg.get('seed', 1)
    links = {ip: Link(ip, lc, seed * 100 + i, loop, upstream, log) for i, (ip, lc) in enumerate(cfg['links'].items())}
    unknown = set(); t_first = [None]
    def event(ev):
        lk = links[ev['link']]
        if 'set' in ev:
            lk.set(ev['set'])
            log.write(json.dumps(dict(event=ev, t=round(loop.time() - t_first[0], 1))) + '\n'); log.flush()
            print('proxy: t=%s link %s set %s' % (ev['t'], ev['link'], ev['set']), file=sys.stderr)
        if 'ramp' in ev:
            dur = float(ev.get('dur', 5)); steps = max(1, int(dur / 0.1))
            start = {k: lk.g(k, lk.g('delay_ms') if k == 'delay_down_ms' else 0.0) for k in ev['ramp']}
            def step(i):
                f = i / steps
                lk.set({k: start[k] + (ev['ramp'][k] - start[k]) * f for k in ev['ramp']})
                if i < steps: loop.call_later(dur / steps, step, i + 1)
                else:
                    log.write(json.dumps(dict(event=ev, done=True, t=round(loop.time() - t_first[0], 1))) + '\n'); log.flush()
            print('proxy: t=%s link %s ramp %s over %ss' % (ev['t'], ev['link'], ev['ramp'], dur), file=sys.stderr)
            step(1)
    def on_listen():
        try:
            while True:
                data, src = ls.recvfrom(2048)
                if t_first[0] is None:
                    t_first[0] = loop.time()
                    for ev in cfg.get('timeline', []):
                        loop.call_at(t_first[0] + ev['t'], event, ev)
                lk = links.get(src[0])
                if lk is None:
                    if src[0] not in unknown:
                        unknown.add(src[0]); print('proxy: dropping packets from unconfigured link %s' % (src,), file=sys.stderr)
                    continue
                lk.up(data, src, ls)
        except (BlockingIOError, InterruptedError):
            pass
    loop.add_reader(ls, on_listen)
    print('proxy: listening on %s -> %s, links %s' % (cfg['listen'], upstream, list(links)), file=sys.stderr)
    t0 = loop.time()
    while True:
        await asyncio.sleep(1.0)
        t = loop.time() - (t_first[0] if t_first[0] is not None else t0)
        for lk in links.values(): lk.flush(t)

if __name__ == '__main__':
    cfg = json.load(open(sys.argv[1]))
    try:
        asyncio.run(main(cfg))
    except KeyboardInterrupt:
        pass
