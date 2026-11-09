// Test receiver: SRTLA listener over the fork's libsrt (like srt-live-server's listener),
// verifies the payload stream (seq/timestamp) and logs SRT + SRTLA stats once a second.
#include "srt.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>

static uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
static uint64_t mono_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
// fields that exist only on newer branches (06620ec+): detected at compile time
template <typename T> auto rcv_quality(const T& p, int) -> decltype(p.pctRcvQuality) { return p.pctRcvQuality; }
template <typename T> double rcv_quality(const T&, long) { return -1; }
template <typename T> auto loss_confirmed(const T& p, int) -> decltype(p.pktRcvLossConfirmed) { return p.pktRcvLossConfirmed; }
template <typename T> int loss_confirmed(const T&, long) { return -1; }

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: receiver PORT [--latency MS] [--lossmaxttl N] [--duration S] [--stats FILE] [--srtla 0|1] [--kbps N] [--debug 0|1]\n"); return 2; }
  int port = atoi(argv[1]); int latency = 2000, lossmaxttl = 50, duration = 40, srtla = 1, kbps = 8000, debug = 0; std::string stats_path;
  for (int i = 2; i + 1 < argc; i += 2) {
    std::string k = argv[i]; const char* v = argv[i+1];
    if (k == "--latency") latency = atoi(v);
    else if (k == "--lossmaxttl") lossmaxttl = atoi(v);
    else if (k == "--duration") duration = atoi(v);
    else if (k == "--stats") stats_path = v;
    else if (k == "--srtla") srtla = atoi(v);
    else if (k == "--kbps") kbps = atoi(v);
    else if (k == "--debug") debug = atoi(v);
  }
  FILE* sf = stats_path.empty() ? stdout : fopen(stats_path.c_str(), "w");
  srt_startup();
  srt_setloglevel(debug ? LOG_DEBUG : LOG_NOTICE);   // notes such as the SRTLA latency raise; --debug 1 for heavy-logging builds
  SRTSOCKET ls = srt_create_socket();
  int yes = 1, no = 0;
  srt_setsockflag(ls, SRTO_LOSSMAXTTL, &lossmaxttl, sizeof lossmaxttl);
  srt_setsockflag(ls, SRTO_LATENCY, &latency, sizeof latency);
  // Receive buffer for bitrate x (latency + 1 s), so 5 s at 25 Mbit does not stall the sender's flow window.
  int pkts = std::max(25600, (int)((double)kbps * 1000 / 8 / 1316 * (latency / 1000.0 + 1.0) * 1.5));
  int rcvbuf = pkts * 1456;
  srt_setsockflag(ls, SRTO_FC, &pkts, sizeof pkts);
  srt_setsockflag(ls, SRTO_RCVBUF, &rcvbuf, sizeof rcvbuf);
  if (srtla && srt_setsockflag(ls, SRTO_SRTLA, &yes, sizeof yes) != 0) { fprintf(stderr, "SRTO_SRTLA failed: %s\n", srt_getlasterror_str()); return 1; }
  srt_setsockflag(ls, SRTO_RCVSYN, &no, sizeof no);
  sockaddr_in sa; memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_port = htons(port); sa.sin_addr.s_addr = htonl(INADDR_ANY);
  if (srt_bind(ls, (sockaddr*)&sa, sizeof sa) != 0) { fprintf(stderr, "bind failed: %s\n", srt_getlasterror_str()); return 1; }
  if (srt_listen(ls, 1) != 0) { fprintf(stderr, "listen failed: %s\n", srt_getlasterror_str()); return 1; }
  fprintf(stderr, "listening on %d (srtla=%d latency=%d lossmaxttl=%d fc=%d rcvbuf=%d)\n", port, srtla, latency, lossmaxttl, pkts, rcvbuf);
  uint64_t deadline = mono_ms() + (uint64_t)duration * 1000;
  SRTSOCKET s = SRT_INVALID_SOCK;
  int eid = srt_epoll_create(); int ev = SRT_EPOLL_IN | SRT_EPOLL_ERR; srt_epoll_add_usock(eid, ls, &ev);
  while (mono_ms() < deadline) {
    SRTSOCKET rd[1]; int rn = 1;
    if (srt_epoll_wait(eid, rd, &rn, NULL, NULL, 100, NULL, NULL, NULL, NULL) <= 0) continue;
    sockaddr_storage peer; int plen = sizeof peer;
    s = srt_accept(ls, (sockaddr*)&peer, &plen);
    if (s != SRT_INVALID_SOCK) break;
  }
  srt_epoll_release(eid);
  if (s == SRT_INVALID_SOCK) { fprintf(stderr, "no connection\n"); return 3; }
  uint64_t t_conn = mono_ms();
  fprintf(stderr, "accepted\n");
  int to = 100; srt_setsockflag(s, SRTO_RCVTIMEO, &to, sizeof to);
  int peer_lat = 0, l = sizeof peer_lat; srt_getsockflag(s, SRTO_RCVLATENCY, &peer_lat, &l);
  fprintf(stderr, "effective rcv latency %d\n", peer_lat);

  char buf[1500];
  uint64_t next_stats = mono_ms() + 1000, t0 = now_us();
  // app-level payload accounting (per second + total)
  long rcvd = 0, missing = 0, dups = 0, ooo = 0, tot_rcvd = 0, tot_missing = 0, tot_dups = 0, tot_ooo = 0, tot_bytes = 0, bytes = 0;
  int64_t next_seq = -1; std::vector<double> lat; lat.reserve(4000);
  double lat_max_all = 0; long lat_over_latency = 0;
  SRT_TRACEBSTATS last_tot; memset(&last_tot, 0, sizeof last_tot);
  while (mono_ms() < deadline) {
    int n = srt_recvmsg2(s, buf, sizeof buf, NULL);
    if (n > 0) {
      uint32_t magic, seq; uint64_t ts; memcpy(&magic, buf, 4); memcpy(&seq, buf + 4, 4); memcpy(&ts, buf + 8, 8);
      if (n >= 16 && magic == 0x53525431) {
        rcvd++; bytes += n;
        double lms = (double)((int64_t)now_us() - (int64_t)ts) / 1000.0; lat.push_back(lms); if (lms > lat_max_all) lat_max_all = lms;
        if (lms > latency + 500) lat_over_latency++;
        if (next_seq < 0) next_seq = seq;
        if ((int64_t)seq == next_seq) next_seq++;
        else if ((int64_t)seq > next_seq) { missing += (int64_t)seq - next_seq; next_seq = seq + 1; }
        else { // older than expected: either a dup or delivered late (TSBPD delivers in order, so treat as reorder)
          ooo++; }
      }
    } else {
      int e = srt_getlasterror(NULL);
      if (e != SRT_EASYNCRCV && e != SRT_ETIMEOUT) { fprintf(stderr, "recv ended: %s\n", srt_getlasterror_str()); break; }
    }
    uint64_t m = mono_ms();
    if (m >= next_stats) {
      next_stats += 1000;
      SRT_TRACEBSTATS p; memset(&p, 0, sizeof p); srt_bstats(s, &p, 1);
      { SRT_TRACEBSTATS c; if (srt_bstats(s, &c, 0) == 0) last_tot = c; }
      std::sort(lat.begin(), lat.end());
      double lp50 = lat.empty() ? 0 : lat[lat.size() / 2], lp99 = lat.empty() ? 0 : lat[(size_t)(lat.size() * 0.99)], lmax = lat.empty() ? 0 : lat.back(), lmin = lat.empty() ? 0 : lat.front();
      SRT_SRTLA_STATS ss; memset(&ss, 0, sizeof ss); srt_srtla_stats(s, &ss);
      fprintf(sf, "{\"t\":%.1f,\"mbpsRecvRate\":%.3f,\"pktRecv\":%lld,\"pktRecvUnique\":%lld,\"pktRcvLoss\":%d,\"pktRcvRetrans\":%d,\"pktRcvDrop\":%d,\"pktRcvBelated\":%lld,\"pktSentNAK\":%d,\"pktSentACK\":%d,\"msRTT\":%.1f,\"pktRcvBuf\":%d,\"msRcvBuf\":%d,\"pktRcvFilterSupply\":%d,\"pctRcvQuality\":%.2f,\"pktRcvLossConfirmed\":%d,\"app_rcvd\":%ld,\"app_missing\":%ld,\"app_ooo\":%ld,\"lat_min\":%.1f,\"lat_p50\":%.1f,\"lat_p99\":%.1f,\"lat_max\":%.1f,\"peers\":[",
        (now_us() - t0) / 1e6, p.mbpsRecvRate, (long long)p.pktRecv, (long long)p.pktRecvUnique, p.pktRcvLoss, p.pktRcvRetrans, p.pktRcvDrop, (long long)p.pktRcvBelated, p.pktSentNAK, p.pktSentACK, p.msRTT, p.pktRcvBuf, p.msRcvBuf, p.pktRcvFilterSupply, rcv_quality(p, 0), loss_confirmed(p, 0), rcvd, missing, ooo, lmin, lp50, lp99, lmax);
      for (int i = 0; i < ss.numPeers; i++) {
        fprintf(sf, "%s{\"id\":%u,\"kbps\":%u,\"kbpsUnique\":%u,\"transitUs\":%d,\"jitterUs\":%u}", i ? "," : "", ss.peers[i].connectionId, ss.peers[i].kbpsRecvRate, ss.peers[i].kbpsRecvUnique, ss.peers[i].transitUs, ss.peers[i].usJitter);
      }
      fprintf(sf, "]}\n"); fflush(sf);
      tot_rcvd += rcvd; tot_missing += missing; tot_dups += dups; tot_ooo += ooo; tot_bytes += bytes; rcvd = missing = dups = ooo = bytes = 0; lat.clear();
    }
  }
  SRT_TRACEBSTATS p = last_tot; { SRT_TRACEBSTATS c; if (srt_bstats(s, &c, 0) == 0) p = c; }
  fprintf(sf, "{\"final\":1,\"app_rcvd\":%ld,\"app_missing\":%ld,\"app_ooo\":%ld,\"app_bytes\":%ld,\"lat_max_all\":%.1f,\"lat_over_latency\":%ld,\"pktRecvTotal\":%lld,\"pktRcvLossTotal\":%d,\"pktRcvDropTotal\":%d,\"pktSentNAKTotal\":%d,\"pktRcvUniqueTotal\":%lld,\"connected_ms\":%llu}\n",
    tot_rcvd, tot_missing, tot_ooo, tot_bytes, lat_max_all, lat_over_latency, (long long)p.pktRecvTotal, p.pktRcvLossTotal, p.pktRcvDropTotal, p.pktSentNAKTotal, (long long)p.pktRecvUniqueTotal, (unsigned long long)(mono_ms() - t_conn));
  fflush(sf);
  srt_close(s); srt_close(ls); srt_cleanup();
  return 0;
}
