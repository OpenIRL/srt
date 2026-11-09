# SRTLA end-to-end harness

Regression harness for the SRTLA receiver in this fork (`srtcore/srtla_rec.*` plus the
SRTLA-gated loss-report logic in `core.cpp` / `list.cpp`). It drives the **real sender
stack** a BELABOX-style encoder uses, so that receiver-side feedback changes (which NAKs go
where, and when) are judged by their effect on the sender: how the links are used, how fast
holes are closed, and how much sits unacknowledged in the send buffer (the signal an
encoder-side bitrate controller reacts to).

```
sender (own code, on BELABOX/srt)  -->  srtla_send (BELABOX/srtla, unmodified)  -->  proxy.py  -->  receiver-<variant> (this repo)
                                         one socket per link (local IPs)             per-link delay/jitter/loss/
                                                                                     rate/outage, no root needed
```

## Licensing

Only code written for this harness lives in this directory. The sender stack is third-party
and is used the way any user would use it, without copying or modifying it:

- `srtla_send` (BELABOX/srtla, AGPL-3) is compiled unmodified straight from a checkout; on
  macOS a compatibility header of our own (`macshim.h`) is passed via `-include`.
- libsrt (BELABOX/srt, MPL-2.0) is built unmodified and linked as a library by our sender.
- belacoder (GPL-3) is not used. Our sender is a constant-bitrate generator that reports the
  unacknowledged send buffer, which is what an adaptive-bitrate controller would read.
- `build.sh` clones both repositories at pinned commits and builds them, locally and in CI; nothing
  is redistributed.

Everything runs on one machine, without root: `srtla_send` binds one socket per local IPv4
address, and the userland proxy keys the links by source IP and forwards each from its own
socket, so the receiver sees distinct link addresses like it would with real modems.

- **Linux** (native, CI, or `docker.sh` on a Mac): the whole `127.0.0.0/8` is routable on
  `lo`, so `127.0.0.1` to `127.0.0.8` are used and every scenario, including the four-link
  real-world set, runs without any setup.
- **macOS native**: only the host's real IPv4 addresses work (loopback, LAN, VM bridge; link-local
  `169.254.x` does not), typically three links. Four-link scenarios refuse to start there; use
  `docker.sh`.

## Files

| File | What |
|---|---|
| `build.sh` | clones BELABOX/srt and BELABOX/srtla at the pinned commits into `build/deps/`; builds `build/receiver-<name>` per `name=ref`, `build/sender`, `build/srtla_send` |
| `receiver.cpp` | SRTLA listener (`SRTO_SRTLA`, `SRTO_LOSSMAXTTL=50` like srt-live-server), checks the payload sequence and one-way latency, logs `srt_bstats` + `srt_srtla_stats` once a second |
| `sender.cpp` | paced constant-bitrate payload generator over BELABOX/srt, configured like a bonded live encoder; 20 ms trace of the unacknowledged send buffer |
| `macshim.h` | compatibility header so the unmodified `srtla_send` compiles on macOS |
| `score.py` | scores a results directory and compares two variants (see CI) |
| `proxy.py` | link impairment proxy: per-direction delay and jitter, FIFO or reordering, random and burst (Gilbert-Elliott) loss, duplicates, rate cap with bounded queue, dead links, timeline `set`/`ramp` events |
| `run_matrix.py` | scenarios, orchestration, per-run summary (`results/<out>/summary.json`) |
| `compare.py` | one table per scenario, variants side by side |
| `repeats_table.py` | aggregate a scenario over several seeds |
| `holes.py` | recovery-time (ACK lag) distribution from the sender's 20 ms buffer trace |
| `losstrace.py` | per-dropped-packet timelines (detect, requests, recovery) from a heavy-logging receiver log |
| `Dockerfile`, `docker.sh` | run build and matrix under Linux, where eight loopback addresses give any number of links |

## Requirements

- git and network access for the first build: `build.sh` clones `BELABOX/srt` (the libsrt fork
  the sender links against) and `BELABOX/srtla` (`srtla_send`) at the pinned commits into
  `build/deps/`. `BELABOX_SRT_REF` and `BELABOX_SRTLA_REF` select other commits.
- cmake (>= 3.19; CLion's bundled one is found automatically on macOS), ninja optional,
  a C/C++ compiler, python3 (no packages).
- At least two non-link-local IPv4 addresses on the host for a 2-link test (a Wi-Fi/LAN
  address plus loopback is enough; a VM bridge gives a third).

## Run

```sh
./build.sh head=HEAD main=main                  # the receiver under test and the reference, the tip of main
./run_matrix.py head,main rw                    # reference catalogue (Linux, 4 links, 2000 ms), ~60 s per run
./run_matrix.py head,main ci --group 1/10       # one tenth of the pull-request set, as a CI job does
./run_matrix.py head,main                       # every scenario except rw_soak
./run_matrix.py head,main lat200_6000,lat200_500 --out lat --seed 2
./compare.py                                    # results/default/summary.json as tables
./repeats_table.py default,s2,s3 heavy_loss     # same scenario over several --out dirs

./docker.sh build head=HEAD main=main           # same, but under Linux in a container
./docker.sh run head,main rw --out linux        # results/linux/ on the host
```

The two variants are the branch under test (`head`) and the tip of `main`, the pair CI scores as
well. `build.sh head=.` builds the working tree as it is, uncommitted changes included.

Per run, `results/<out>/<scenario>/<variant>/` holds `snd.jsonl` (sender, 1 Hz: send rate,
`bs` = unacked send buffer, RTT, retransmissions), `snd.bstrace` (the same buffer every 20 ms),
`rcv.jsonl` (receiver: NAKs sent, retransmissions received, TLPKTDROPs, belated packets,
application gaps, one-way latency percentiles, per-link SRTLA stats) and `proxy.jsonl` (per
link and second: forwarded / lost / queue-dropped packets, current impairment).

Ports 15000-17000 are used; `test-srt` uses 5000+, so both can run at once (mind the CPU).

Loss traces: `CMAKE_EXTRA=-DENABLE_HEAVY_LOGGING=ON ./build.sh head=.` builds a receiver that
logs every loss record's detection, requests and recovery, `RECEIVER_DEBUG=1 ./run_matrix.py ...`
runs it at debug level, and `./losstrace.py results/<out>/<scenario>/head/rcv.err` builds the
per-dropped-packet timelines. Both variables pass through `docker.sh`.

`build/wt/<name>` are git worktrees of this repository (`git worktree remove build/wt/<name>`
to drop one). Keep the machine otherwise idle during runs: a 1-2 s stall of the host shows up
as hundreds of sender drops and an e2e p99 far above the latency, and is not a receiver result.

## Scenario catalogue

The `rw_*` scenarios are generated from 15 templates, each the four-link picture of one thing a
bonded mobile encoder meets on the road. Every template runs with any link set, link count and
latency; the reference is four decent carriers (25/40/55/70 ms one-way, 0.5 % loss) at 2000 ms.

| template | real-world situation |
|---|---|
| `baseline` | healthy modems |
| `one_slow` / `two_slow` | one (two) carriers with a far higher base RTT: +200 ms (+160 / +240 ms) |
| `bloat` | bufferbloat: one link's delay ramps +560 ms over 8 s, holds, drains back |
| `bursty` | Gilbert-Elliott burst loss (~10-packet bursts) on two links |
| `flapping` | one modem drops out for 4 s every 10 s |
| `handover` | 3 s outage every 15 s on the fastest link (cell handover, short tunnel) |
| `join_late` | a modem boots 15 s into the stream and registers while streaming |
| `asym_dir` | uplink as configured but downlink +90 ms on every link (the NAK/ACK path is the slow one) |
| `reorder` | HARQ-style reordering inside every link (15 ms non-FIFO jitter) |
| `dup` | 2 % duplicated packets on one link |
| `capacity` | 2 Mbit; every link capped at 3 Mbit, the bond shrinks to 1.6 Mbit for 15 s: queues overflow |
| `dead_zone` | 2 Mbit; all links get 20 % loss and +300 ms for 15 s, then recover |
| `highrate` / `lowrate` | 25 Mbit; 400 kbit with 2 % loss on two links |

Dimensions and sets (`run_matrix.py VARIANTS <set>`):

| set | what | scenarios |
|---|---|---|
| `rw` | the reference: four carriers, 2000 ms | 15 |
| `rw-lat` | the catalogue at 1000, 3000, 4000, 5000 ms (`_lat<N>`) | 60 |
| `rw-slow` | the catalogue on slow carriers 100/120/140/160 ms (`_slow`) | 15 |
| `rw-links` | the catalogue with 2, 3, 5, 6, 7, 8 links (`_links<N>`; delays 25 + 15 ms per link) | 90 |
| `rw-extra` | one link 1.1 s / 2 s late, a bloat ramp to 2 s, the slow link listed first (TSBPD time base) | 4 |
| `loss` | the lossy scenarios that separate algorithms (the clean ones any algorithm passes): dead zone in two strengths, 8 % on all links, 15 % on two, bursts on all four, handover with loss, capacity, a slow link with loss, 4 % at the 1000 ms floor | 10 |
| `ci` | the pull-request set: `rw` plus the extremes of each dimension on the whole catalogue: 5000 ms, 2 links, 8 links, slow carriers, and `rw_floor` (both sides ask for 500 ms; the receiver must raise to the SRTLA minimum of 1000 ms). What holds at both ends is assumed to hold in between. | 76 |
| `full` | everything except the 5-minute `rw_soak` | 195 |

SRTLA does not run below 1000 ms receiver latency (protocol rule, see `SRTO_SRTLA` in
`docs/API/API-socket-options.md`; `score.py` flags any run below it) and encoders use 2000 (BELABOX)
or 3000 ms; the catalogue covers 1000 to 5000. The two- and three-link scenarios (`clean2`, `asym_rtt`,
`loss_both`, `heavy_loss`, `outage`, `thin_link`, `highrate`, `jitter_reorder`, `three_*`, `lat*`)
stay for macOS-native runs. Not covered: a Moblin sender
(iOS), several groups on one listener, NAT rebinding of a link mid-stream.


## CI: score and pull-request comment

`.github/workflows/srtla-e2e.yml` runs on pull requests that touch `srtcore/` or this
directory (and on demand). Ten parallel Ubuntu jobs each build two receivers from this
repository, the pull request (merge commit) and the tip of `main`, plus libsrt and `srtla_send` from
the pinned BELABOX/srt and BELABOX/srtla commits, and run their share of the `ci` set (75 scenarios) with two seeds, base and
head interleaved so both see the same machine. A final job merges the results, runs `score.py`,
writes the scorecard to the job summary and posts (or updates) one comment on the pull request.
A regression fails the check.

`score.py` gives every scenario and variant five sub-scores from 0 to 100:

| sub-score | weight | measures |
|---|---|---|
| delivery | 0.40 | packets the application lost: `100 * exp(-lost permille / 5)`: one permille = 82 points, five = 37, twenty-five = 1 |
| efficiency | 0.20 | share of retransmissions that were duplicates (a request the sender paid for twice) |
| bond | 0.15 | the weakest healthy link's share against half of its fair share: does every usable link stay in the bond |
| recovery | 0.15 | ACK-lag p99 against half the latency: how fast the oldest hole gets closed |
| latency | 0.10 | end-to-end latency beyond what the fastest link allows, against 500 ms |

The total per variant is the mean over scenarios. The verdict is **regression** when the total
drops by more than 3, any scenario by more than 10, the application lost 3+ packets where the
base lost none, or more than twice what the base lost. Runs during which the host stalled
(proxy ticks more than 1.5 s apart) are excluded and listed. Locally:

```sh
./run_matrix.py main,head ci --out s1 --seed 1 && ./run_matrix.py main,head ci --out s2 --seed 2
./score.py s1,s2 --base main --head head
```

Pull requests from forks get the check result and the job summary, but no comment (the token
is read-only there).

## Reading the numbers

- `missing` / `rcvdrop` / `snddrop`: application gaps, receiver TLPKTDROPs, sender drops.
  Any non-zero value is a hard regression.
- `NAKs`, `rexmit`, `belated`: loss-report packets sent, retransmissions the sender made,
  retransmissions that arrived for packets already recovered (duplicate requests).
- `bs mean` / `bs max`: unacknowledged packets in the sender's buffer (`SRTO_SNDDATA`). This is
  what an encoder-side bitrate controller reacts to; it grows with the age of the oldest unrecovered hole.
- `share`: per-link share of forwarded uplink packets. A bond that collapses onto one link
  shows here.
- `lag p99`: ACK lag, the age of the oldest unacknowledged packet (from the 20 ms trace), i.e.
  how long the oldest hole stayed open. `stall`: longest gap between proxy ticks; a host stall
  invalidates the run. `exit.json` records the exit code of all four processes; a run whose
  stream died early is excluded from the score the same way.
