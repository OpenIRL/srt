/*
 * srtla_rec — SRTLA receiver demux for libsrt
 * Copyright (c) 2026 OpenIRL
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * This file is part of the SRTLA extension for libsrt. It is licensed under
 * the GNU Affero General Public License, version 3.0 only (AGPL-3.0-only):
 * you can redistribute it and/or modify it under the terms of the AGPL as
 * published by the Free Software Foundation, version 3. It is distributed
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See <https://www.gnu.org/licenses/>.
 *
 * Scope note: only the srtla_rec.* SRTLA-extension files are AGPL-licensed;
 * the rest of libsrt remains under the Mozilla Public License 2.0. This is an
 * original, independent implementation of the SRTLA receiver protocol. See
 * LICENSING.md.
 *
 * This file implements the receiver ("demux") side of SRTLA (SRT Link
 * Aggregation) as a channel-level shim below SRT. The demux terminates the
 * bonded UDP links directly in libsrt's receive path: it runs the SRTLA
 * registration handshake, aggregates many source addresses (one per modem/link)
 * onto a single CUDT, generates per-link SRTLA-ACKs and keepalive echoes, and
 * fans SRT ACK/NAK feedback out over every link. See queue.cpp / core.cpp for
 * the concrete ingress / egress / binding hook points.
 */

#ifndef INC_SRT_SRTLA_REC_H
#define INC_SRT_SRTLA_REC_H

#include <cstdint>
#include <cstring>
#include <list>
#include <map>
#include <vector>

#include "netinet_any.h"
#include "sync.h"
#include "udt.h" // SRTSOCKET

namespace srt
{

class CChannel;
class CPacket;
struct CUnit;
class CUDT;

/// SRTLA receiver demux / registry. One instance per SRTLA-designated multiplexer,
/// shared by the receive worker (ingress), the send worker (egress fan-out) and the
/// receive worker's periodic timing pass (cleanup/keepalive/stats). All access is
/// serialized by m_Lock.
class SrtlaRec
{
public:
    // ---- Protocol constants (spec §7) ----
    static const size_t   SRTLA_ID_LEN         = 256; // full group id (128 client + 128 server)
    static const size_t   SRTLA_ID_HALF        = 128;
    static const size_t   REG_LEN              = 2 + SRTLA_ID_LEN; // REG1/REG2 datagram length (258)
    static const size_t   SRTLA_ACK_LEN        = 44; // header word + 10 SNs
    static const size_t   SRT_MIN_LEN          = 16; // min length treated as an SRT packet
    static const size_t   MIN_PAD              = 32; // short control packets padded to this
    static const int      RECV_ACK_INT         = 10; // SRT data packets per SRTLA ACK (batch cap)
    static const int64_t  ACK_FLUSH_US         = 30000; // partial-batch flush age
    static const size_t   MAX_CONNS_PER_GROUP  = 16; // == SRT_SRTLA_MAX_PEERS
    static const size_t   MAX_GROUPS           = 200;
    static const int32_t  SN_WINDOW_SIZE       = 65536; // retransmission-tracking bitmap
    static const int32_t  SN_WINDOW_MASK       = SN_WINDOW_SIZE - 1;

    // ---- Wire type values (first big-endian uint16) (spec §2.1) ----
    static const uint16_t T_SRT_HANDSHAKE = 0x8000;
    static const uint16_t T_SRT_ACK       = 0x8002;
    static const uint16_t T_SRT_NAK       = 0x8003;
    static const uint16_t T_KEEPALIVE     = 0x9000;
    static const uint16_t T_SRTLA_ACK     = 0x9100;
    static const uint16_t T_REG1          = 0x9200;
    static const uint16_t T_REG2          = 0x9201;
    static const uint16_t T_REG3          = 0x9202;
    static const uint16_t T_REG_ERR       = 0x9210;
    static const uint16_t T_REG_NGP       = 0x9211;

    enum Ingress
    {
        SRTLA_DROP, // fully consumed by the demux; do not dispatch further
        SRTLA_PASS  // SRT traffic from a registered link; let the worker dispatch it
    };

    explicit SrtlaRec(CChannel* chan);
    ~SrtlaRec();

    /// Ingress hook (receive worker). Classifies one datagram by its source address
    /// and first bytes and either handles it (registration / keepalive / SRTLA-ACK
    /// bookkeeping → SRTLA_DROP) or lets the SRT packet through (SRTLA_PASS).
    Ingress onIngress(const sockaddr_any& src, CUnit* unit);

    /// Egress hook (send worker). For a packet destined to an SRTLA group's peer:
    /// fans SRT ACK/NAK out to every link, sends everything else to the group's most
    /// recently active link, and returns true (the demux performed the send). Returns
    /// false for non-SRTLA destinations so the caller sends normally.
    bool onEgress(const sockaddr_any& peer, CPacket& pkt, const sockaddr_any& src);

    /// Binding moment (listener accept path): associate the just-created CUDT with the
    /// SRTLA group whose registered link is @a peer, so all links route to it and the
    /// reverse path can find the group.
    void bindGroup(const sockaddr_any& peer, CUDT* u);

    /// Periodic pass (receive worker timing section): link/group timeouts and recovery
    /// keepalives. Internally throttled.
    void onPeriodic(const sync::steady_clock::time_point& now);

    /// On-demand stats: fill @a out with the live per-link values (continuously
    /// maintained EWMAs, read fresh) of the SRTLA group bound to @a socket_id.
    /// Returns true (out.valid = 1) if such a group exists.
    bool fillStats(SRTSOCKET socket_id, SRT_SRTLA_STATS* out);

private:
    typedef sync::steady_clock::time_point time_point;

    struct Link
    {
        sockaddr_any addr;
        time_point   last_rcvd;
        time_point   established;
        time_point   recovery_start;
        time_point   last_keepalive;
        bool         recovering;

        // SRTLA-ACK batch: raw 31-bit SNs in receive order (spec §3.5).
        uint32_t   ack_log[RECV_ACK_INT];
        int        ack_count;
        time_point ack_first_pending; // when the oldest unsent entry was queued

        uint32_t connectionId;         // FNV-1a of addr+port

        // Time-weighted exponential rate estimators (decayed at read time).
        double     recv_acc;           // all received wire bytes (data+retransmit+control)
        double     unique_acc;         // first-seen (non-duplicate) wire bytes
        time_point rate_last;
        bool       rate_init;

        double     transit_ewma;       // smoothed relative one-way transit, us (RTT anchor)
        time_point last_transit;
        bool       transit_valid;
        double     jitter_ewma;        // smoothed jitter, us (RFC 3550)
        int64_t    prev_transit;
        bool       have_prev_transit;

        Link()
            : recovering(false)
            , ack_count(0)
            , connectionId(0)
            , recv_acc(0.0)
            , unique_acc(0.0)
            , rate_init(false)
            , transit_ewma(0.0)
            , transit_valid(false)
            , jitter_ewma(0.0)
            , prev_transit(0)
            , have_prev_transit(false)
        {
        }
    };

    struct Group
    {
        uint8_t         id[SRTLA_ID_LEN];
        std::list<Link> links;
        SRTSOCKET       socket_id; // bound CUDT socket id, or SRT_INVALID_SOCK
        bool            bound;
        time_point      created_at;
        sockaddr_any    last_addr;   // most recently active link (reverse path primary)
        bool            has_last_addr;
        bool            data_seen;

        // Retransmission sliding window (unique-byte accounting, spec §4.5).
        std::vector<bool> sn_window;
        int32_t           sn_max;
        bool              sn_valid;

        // Shared reference for per-link relative transit; sender timestamp unwrapped
        // with signed 32-bit deltas (wrap- and reorder-safe).
        bool       ts_init;
        uint32_t   ts_last_raw;
        int64_t    sender_ts_acc;  // unwrapped sender time since reference, us
        time_point arrival_ref;

        Group()
            : socket_id(-1)
            , bound(false)
            , has_last_addr(false)
            , data_seen(false)
            , sn_window(SN_WINDOW_SIZE, false)
            , sn_max(0)
            , sn_valid(false)
            , ts_init(false)
            , ts_last_raw(0)
            , sender_ts_acc(0)
        {
            memset(id, 0, sizeof id);
        }
    };

    struct LinkRef
    {
        Group* g;
        Link*  l;
        LinkRef(): g(NULL), l(NULL) {}
        LinkRef(Group* gg, Link* ll): g(gg), l(ll) {}
        bool valid() const { return g != NULL && l != NULL; }
    };

    // Const comparator for address-keyed maps, ordering by (family, address, port)
    // only (sockaddr_any::Less has a non-const operator() and can't be a map comparator).
    struct AddrLess
    {
        bool operator()(const sockaddr_any& a, const sockaddr_any& b) const
        {
            if (a.family() != b.family())
                return a.family() < b.family();
            if (a.family() == AF_INET)
            {
                if (a.sin.sin_addr.s_addr != b.sin.sin_addr.s_addr)
                    return a.sin.sin_addr.s_addr < b.sin.sin_addr.s_addr;
                return a.sin.sin_port < b.sin.sin_port;
            }
            const int c = memcmp(&a.sin6.sin6_addr, &b.sin6.sin6_addr, sizeof(in6_addr));
            if (c != 0)
                return c < 0;
            return a.sin6.sin6_port < b.sin6.sin6_port;
        }
    };

    // --- registration state machine (spec §4.2) ---
    void handleReg1(const sockaddr_any& src, const uint8_t* wire);
    void handleReg2(const sockaddr_any& src, const uint8_t* wire);

    // --- lookups ---
    LinkRef findLink(const sockaddr_any& src);
    Group*  findGroupById(const uint8_t* full_id);
    Group*  findGroupByEgress(const sockaddr_any& peer);
    Group*  findGhostCreator(const sockaddr_any& src);

    // --- feedback senders (SRTLA-native, raw wire bytes) ---
    void sendReg2(const sockaddr_any& dst, const Group& g);
    void sendReg3(const sockaddr_any& dst);
    void sendShort(const sockaddr_any& dst, uint16_t type); // REG3/ERR/NGP/keepalive (padded to 32)
    void sendSrtlaAck(const sockaddr_any& dst, Link& l);
    void echoKeepalive(const sockaddr_any& dst, CPacket& pkt);
    void sendKeepalive(const sockaddr_any& dst);

    // --- accounting helpers ---
    bool trackSn(Group& g, int32_t sn);            // returns true if unique (first seen)

    // --- cleanup ---
    void doCleanup(const time_point& now);
    void removeGroup(std::list<Group>::iterator git);

    // --- utilities ---
    static uint16_t peekType(const CPacket& pkt);
    static size_t   rebuildWire(const CPacket& pkt, uint8_t* out, size_t outcap);
    static uint32_t fnv1a(const sockaddr_any& addr);
    static bool     ctEqual(const uint8_t* a, const uint8_t* b, size_t n);
    static void     fillRandom(uint8_t* out, size_t n);

    CChannel*    m_pChannel;
    sync::Mutex  m_Lock;

    std::list<Group>                          m_Groups;
    std::map<sockaddr_any, LinkRef, AddrLess> m_LinkMap;   // link addr → (group, link)
    std::map<sockaddr_any, Group*, AddrLess>  m_EgressMap; // CUDT peer addr → group (bind lifetime)

    time_point m_LastCleanup;
    bool       m_HaveTimers;

    SrtlaRec(const SrtlaRec&);
    SrtlaRec& operator=(const SrtlaRec&);
};

} // namespace srt

#endif // INC_SRT_SRTLA_REC_H
