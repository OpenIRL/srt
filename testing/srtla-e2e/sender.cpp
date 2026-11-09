// Test sender for the SRTLA end-to-end harness. Sends a paced constant-bitrate payload
// stream (sequence number + timestamp per packet) through the BELABOX libsrt fork (BELABOX/srt),
// configured like a bonded live encoder (SRTO_MAXBW 0, SRTO_OHEADBW 20, SRTO_RETRANSMITALGO 1).
// It reports the unacknowledged send buffer (SRTO_SNDDATA) once a second, the signal an
// adaptive-bitrate controller on the encoder side reacts to.
#include "srt.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <chrono>
#include <thread>
#include <algorithm>
#include <netdb.h>
#include <sys/socket.h>

static uint64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
static uint64_t mono_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static SRTSOCKET sock;
static int srt_latency = 2000;
static int max_bitrate = 6000 * 1000;
static int cur_bitrate;
static const int BITRATE_UPDATE_INT = 20; // ms between stats polls (an encoder-side controller polls at this rate)

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: sender HOST PORT [--bitrate KBPS] [--duration S] [--latency MS] [--stats FILE] [--bstrace FILE]\n"); return 2; }
  const char* host = argv[1]; const char* port = argv[2];
  int duration = 30; std::string stats_path, trace_path;
  for (int i = 3; i + 1 < argc; i += 2) {
    std::string k = argv[i]; const char* v = argv[i+1];
    if (k == "--bitrate") max_bitrate = atoi(v) * 1000;
    else if (k == "--duration") duration = atoi(v);
    else if (k == "--latency") srt_latency = atoi(v);
    else if (k == "--stats") stats_path = v;
    else if (k == "--bstrace") trace_path = v;
  }
  cur_bitrate = max_bitrate;
  FILE* sf = stats_path.empty() ? stdout : fopen(stats_path.c_str(), "w");
  FILE* tf = trace_path.empty() ? NULL : fopen(trace_path.c_str(), "w");

  srt_startup();
  struct addrinfo hints; memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
  struct addrinfo* addrs; if (getaddrinfo(host, port, &hints, &addrs) != 0) { fprintf(stderr, "resolve failed\n"); return 1; }
  sock = srt_create_socket();
  int64_t max_bw = 0; srt_setsockflag(sock, SRTO_MAXBW, &max_bw, sizeof max_bw);
  int32_t ohead = 20; srt_setsockflag(sock, SRTO_OHEADBW, &ohead, sizeof ohead);
  srt_setsockflag(sock, SRTO_LATENCY, &srt_latency, sizeof srt_latency);
  int32_t algo = 1; srt_setsockflag(sock, SRTO_RETRANSMITALGO, &algo, sizeof algo);
  // Send buffer for bitrate x (latency + 1 s): packets stay until acknowledged, so 5 s at 25 Mbit
  // needs far more than the default 8192.
  int pkts = std::max(25600, (int)((double)max_bitrate / 8 / 1316 * (srt_latency / 1000.0 + 1.0) * 1.5));
  int sndbuf = pkts * 1456;
  srt_setsockflag(sock, SRTO_FC, &pkts, sizeof pkts);
  srt_setsockflag(sock, SRTO_SNDBUF, &sndbuf, sizeof sndbuf);
  const char* sid = "test"; srt_setsockflag(sock, SRTO_STREAMID, sid, strlen(sid));
  int conn_to = 10000; srt_setsockflag(sock, SRTO_CONNTIMEO, &conn_to, sizeof conn_to);

  uint64_t t_connect0 = mono_ms();
  if (srt_connect(sock, addrs->ai_addr, addrs->ai_addrlen) != 0) { fprintf(stderr, "connect failed: %s\n", srt_getlasterror_str()); return 1; }
  int len = sizeof srt_latency; srt_getsockflag(sock, SRTO_PEERLATENCY, &srt_latency, &len);
  fprintf(stderr, "SRT connected after %llu ms. Negotiated latency: %d ms\n", (unsigned long long)(mono_ms() - t_connect0), srt_latency);
  bool nb = false; srt_setsockflag(sock, SRTO_SNDSYN, &nb, sizeof nb);

  static char pkt[1316]; memset(pkt, 0xAB, sizeof pkt);
  uint32_t seq = 0;
  double credit = 0; uint64_t last = now_us(); uint64_t t0 = last;
  uint64_t next_poll = mono_ms() + BITRATE_UPDATE_INT, next_stats = mono_ms() + 1000;
  long a_sent = 0, a_retrans = 0, a_snddrop = 0, a_nak = 0, a_ack = 0, app_drop = 0, app_sent = 0;
  double last_rtt = 0, last_rate = 0; int last_flight = 0, bs = 0; long last_availbuf = 0;
  while (true) {
    uint64_t t = now_us();
    if (t - t0 >= (uint64_t)duration * 1000000ULL) break;
    credit += (double)cur_bitrate / 8.0 * (double)(t - last) / 1e6; last = t;
    if (credit > 4.0 * 1316) credit = 4.0 * 1316;
    while (credit >= 1316) {
      credit -= 1316;
      uint32_t magic = 0x53525431; memcpy(pkt, &magic, 4); memcpy(pkt + 4, &seq, 4); uint64_t ts = now_us(); memcpy(pkt + 8, &ts, 8);
      int r = srt_sendmsg2(sock, pkt, sizeof pkt, NULL);
      if (r != (int)sizeof pkt) { app_drop++; if (srt_getlasterror(NULL) != SRT_EASYNCSND) { fprintf(stderr, "send error: %s\n", srt_getlasterror_str()); goto out; } }
      else app_sent++;
      seq++;
    }
    uint64_t m = mono_ms();
    if (m >= next_poll) {
      next_poll += BITRATE_UPDATE_INT;
      SRT_TRACEBSTATS st; if (srt_bstats(sock, &st, 1) == 0) {
        a_sent += st.pktSent; a_retrans += st.pktRetrans; a_snddrop += st.pktSndDrop; a_nak += st.pktRecvNAK; a_ack += st.pktRecvACK;
        last_rtt = st.msRTT; last_rate = st.mbpsSendRate; last_flight = st.pktFlightSize; last_availbuf = st.byteAvailSndBuf;
        int sz = sizeof(bs); if (srt_getsockflag(sock, SRTO_SNDDATA, &bs, &sz) != 0) bs = -1;
        if (tf) fprintf(tf, "%.3f %d %d %d\n", (now_us() - t0) / 1e6, bs, st.pktFlightSize, st.pktRetrans);
      }
    }
    if (m >= next_stats) {
      next_stats += 1000;
      fprintf(sf, "{\"t\":%.1f,\"bitrate_kbps\":%d,\"mbpsSendRate\":%.3f,\"pktSent\":%ld,\"pktRetrans\":%ld,\"pktSndDrop\":%ld,\"pktRecvNAK\":%ld,\"pktRecvACK\":%ld,\"msRTT\":%.1f,\"pktFlightSize\":%d,\"bs\":%d,\"byteAvailSndBuf\":%ld,\"app_sent\":%ld,\"app_drop\":%ld}\n",
        (t - t0) / 1e6, cur_bitrate / 1000, last_rate, a_sent, a_retrans, a_snddrop, a_nak, a_ack, last_rtt, last_flight, bs, last_availbuf, app_sent, app_drop);
      fflush(sf);
      a_sent = a_retrans = a_snddrop = a_nak = a_ack = 0; app_sent = app_drop = 0;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(250));
  }
out:
  // Let everything in flight be delivered and acknowledged before closing, so a loss in the
  // last round trip is recovered instead of being counted as a teardown drop.
  std::this_thread::sleep_for(std::chrono::milliseconds(srt_latency + 1000));
  SRT_TRACEBSTATS tot; if (srt_bstats(sock, &tot, 0) == 0) {
    fprintf(sf, "{\"final\":1,\"seq\":%u,\"pktSentTotal\":%lld,\"pktRetransTotal\":%d,\"pktSndDropTotal\":%d,\"pktRecvNAKTotal\":%d,\"pktSndLossTotal\":%d}\n", seq, (long long)tot.pktSentTotal, tot.pktRetransTotal, tot.pktSndDropTotal, tot.pktRecvNAKTotal, tot.pktSndLossTotal);
    fflush(sf);
  }
  srt_close(sock); srt_cleanup();
  return 0;
}
