/*
 * srtla_rec — SRTLA receiver demux for libsrt
 * Copyright (c) 2026 OpenIRL
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * AGPL-3.0-only. See srtla_rec.h for the full license header and LICENSING.md
 * (repo root) for how it scopes against the MPL-2.0 base.
 * Original, independent implementation of the SRTLA receiver protocol.
 */

#include "platform_sys.h"

#include <random>

#include "srtla_rec.h"
#include "channel.h"
#include "packet.h"
#include "queue.h"   // CUnit
#include "core.h"    // CUDT
#include "api.h"     // CUDTUnited, SocketKeeper
#include "logging.h"

using namespace srt;
using namespace srt::sync;
using namespace srt_logging;

namespace
{
// Inverse time constant (~1 s) for the per-link EWMAs / rate decay (time-weighted).
const double EWMA_INV_TAU = 1.0 / 1000000.0;

inline void store_be16(uint8_t* p, uint16_t v)
{
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}
inline void store_be32(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}
} // namespace

// --------------------------------------------------------------------------
// Construction
// --------------------------------------------------------------------------

srt::SrtlaRec::SrtlaRec(CChannel* chan)
    : m_pChannel(chan)
    , m_HaveTimers(false)
{
}

srt::SrtlaRec::~SrtlaRec() {}

// --------------------------------------------------------------------------
// Utilities
// --------------------------------------------------------------------------

uint16_t srt::SrtlaRec::peekType(const CPacket& pkt)
{
    // The 16-byte header has been converted to host byte order by the channel.
    // The wire "type" field is the top 16 bits of the first header word, which is
    // endianness-independent once the word is a host-order integer.
    return uint16_t((const_cast<CPacket&>(pkt).getHeader()[SRT_PH_SEQNO] >> 16) & 0xFFFF);
}

// Reconstruct the network-order (wire) datagram from a CPacket the channel already
// converted to host order (reverses toHostByteOrder's word swaps). Needed for opaque
// byte arrays (group ids, verbatim keepalives). Returns the datagram length in @a out.
size_t srt::SrtlaRec::rebuildWire(const CPacket& pkt, uint8_t* out, size_t outcap)
{
    CPacket&     p    = const_cast<CPacket&>(pkt);
    const size_t plen = p.getLength();
    // Test plen before adding 16 so a sub-header length underflow cannot wrap the sum.
    if (plen > outcap)
        return 0;
    const size_t total = 16 + plen;
    if (total > outcap)
        return 0;

    const uint32_t* hdr = p.getHeader();
    for (int i = 0; i < 4; ++i)
        store_be32(out + 4 * i, hdr[i]);

    const uint8_t* pl = (const uint8_t*)p.data();
    if (p.isControl())
    {
        const size_t nwords = plen / 4;
        const uint32_t* plw = (const uint32_t*)p.data();
        for (size_t w = 0; w < nwords; ++w)
            store_be32(out + 16 + 4 * w, plw[w]);
        // trailing bytes (plen % 4) were never word-swapped; copy verbatim
        for (size_t b = nwords * 4; b < plen; ++b)
            out[16 + b] = pl[b];
    }
    else
    {
        memcpy(out + 16, pl, plen);
    }
    return total;
}

// FNV-1a 32-bit over the link's address bytes + port (network order as stored).
// For IPv4 this hashes addr(4)+port(2), byte-identical to the reference (spec §7);
// IPv6 hashes the full address + port.
uint32_t srt::SrtlaRec::fnv1a(const sockaddr_any& addr)
{
    uint32_t hash = 0x811c9dc5u;
    const uint8_t* bytes;
    size_t         nbytes;
    uint16_t       port_net;

    if (addr.family() == AF_INET)
    {
        bytes    = (const uint8_t*)&addr.sin.sin_addr;
        nbytes   = 4;
        port_net = addr.sin.sin_port;
    }
    else
    {
        bytes    = (const uint8_t*)&addr.sin6.sin6_addr;
        nbytes   = 16;
        port_net = addr.sin6.sin6_port;
    }

    for (size_t i = 0; i < nbytes; ++i)
        hash = (hash ^ bytes[i]) * 0x01000193u;

    const uint8_t* pp = (const uint8_t*)&port_net;
    hash = (hash ^ pp[0]) * 0x01000193u;
    hash = (hash ^ pp[1]) * 0x01000193u;
    return hash;
}

// Constant-time comparison of two group ids (no early-out) to avoid timing side
// channels on the 256-byte id (spec §7).
bool srt::SrtlaRec::ctEqual(const uint8_t* a, const uint8_t* b, size_t n)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i)
        diff |= uint8_t(a[i] ^ b[i]);
    return diff == 0;
}

void srt::SrtlaRec::fillRandom(uint8_t* out, size_t n)
{
    // CSPRNG for the server half of the group id. std::random_device is a
    // non-deterministic source on the supported platforms.
    static std::random_device rd;
    size_t i = 0;
    while (i < n)
    {
        uint32_t r = rd();
        for (int b = 0; b < 4 && i < n; ++b, ++i)
            out[i] = uint8_t(r >> (8 * b));
    }
}

// --------------------------------------------------------------------------
// Lookups
// --------------------------------------------------------------------------

srt::SrtlaRec::LinkRef srt::SrtlaRec::findLink(const sockaddr_any& src)
{
    std::map<sockaddr_any, LinkRef, AddrLess>::iterator it = m_LinkMap.find(src);
    if (it == m_LinkMap.end())
        return LinkRef();
    return it->second;
}

srt::SrtlaRec::Group* srt::SrtlaRec::findGroupById(const uint8_t* full_id)
{
    Group* found = NULL;
    for (std::list<Group>::iterator g = m_Groups.begin(); g != m_Groups.end(); ++g)
    {
        // Constant-time over each candidate; do not early-out on the first match
        // either, to keep the number of comparisons independent of position.
        if (ctEqual(g->id, full_id, SRTLA_ID_LEN))
            found = &*g;
    }
    return found;
}

srt::SrtlaRec::Group* srt::SrtlaRec::findGroupByEgress(const sockaddr_any& peer)
{
    std::map<sockaddr_any, Group*, AddrLess>::iterator it = m_EgressMap.find(peer);
    if (it == m_EgressMap.end())
        return NULL;
    return it->second;
}

// A "ghost" creator: a group that this src created via REG1 but which has no links
// yet and has never carried data. Used to make a retransmitted REG1 idempotent.
srt::SrtlaRec::Group* srt::SrtlaRec::findGhostCreator(const sockaddr_any& src)
{
    for (std::list<Group>::iterator g = m_Groups.begin(); g != m_Groups.end(); ++g)
    {
        if (g->links.empty() && !g->data_seen && g->has_last_addr && g->last_addr == src)
            return &*g;
    }
    return NULL;
}

// --------------------------------------------------------------------------
// Feedback senders (SRTLA-native, raw wire bytes)
// --------------------------------------------------------------------------

void srt::SrtlaRec::sendReg2(const sockaddr_any& dst, const Group& g)
{
    uint8_t buf[REG_LEN];
    store_be16(buf, T_REG2);
    memcpy(buf + 2, g.id, SRTLA_ID_LEN);
    m_pChannel->sendtoRaw(dst, (const char*)buf, REG_LEN);
}

void srt::SrtlaRec::sendReg3(const sockaddr_any& dst)
{
    sendShort(dst, T_REG3);
}

// Short control packet: 2-byte type zero-padded to MIN_PAD (32) bytes (spec §2.2).
void srt::SrtlaRec::sendShort(const sockaddr_any& dst, uint16_t type)
{
    uint8_t buf[MIN_PAD];
    memset(buf, 0, sizeof buf);
    store_be16(buf, type);
    m_pChannel->sendtoRaw(dst, (const char*)buf, MIN_PAD);
}

void srt::SrtlaRec::sendKeepalive(const sockaddr_any& dst)
{
    sendShort(dst, T_KEEPALIVE);
}

// SRTLA ACK (spec §3.5): 44 bytes = header word 0x91000000 followed by 10 acked
// SRT data sequence numbers (big-endian, as received).
void srt::SrtlaRec::sendSrtlaAck(const sockaddr_any& dst, Link& l)
{
    if (l.ack_count <= 0)
        return;

    // Variable-length ACK: header word + one word per pending SN.
    uint8_t buf[SRTLA_ACK_LEN];
    store_be32(buf, uint32_t(T_SRTLA_ACK) << 16); // 0x91000000
    const int n = l.ack_count;
    for (int i = 0; i < n; ++i)
        store_be32(buf + 4 + 4 * i, l.ack_log[i]);
    m_pChannel->sendtoRaw(dst, (const char*)buf, 4 + 4 * (size_t)n);
    l.ack_count = 0;
}

// Echo a received keepalive verbatim, padded to at least MIN_PAD (spec §3.4).
void srt::SrtlaRec::echoKeepalive(const sockaddr_any& dst, CPacket& pkt)
{
    uint8_t buf[1500];
    memset(buf, 0, sizeof buf);
    size_t len = rebuildWire(pkt, buf, sizeof buf);
    if (len == 0)
    {
        // Datagram too short to reconstruct (channel length underflow) or oversized:
        // the payload is opaque, so a minimal padded keepalive is an equivalent reply.
        sendKeepalive(dst);
        return;
    }
    if (len < MIN_PAD)
        len = MIN_PAD;
    m_pChannel->sendtoRaw(dst, (const char*)buf, len);
}

// --------------------------------------------------------------------------
// Registration state machine (spec §4.2)
// --------------------------------------------------------------------------

void srt::SrtlaRec::handleReg1(const sockaddr_any& src, const uint8_t* wire)
{
    // Rule 1: a source already registered as a link may not (re-)REG1.
    if (findLink(src).valid())
    {
        HLOGC(cnlog.Debug, log << "SRTLA REG1 from already-registered link " << src.str() << " -> REG_ERR");
        sendShort(src, T_REG_ERR);
        return;
    }

    // Idempotent retransmit: this src already created a still-empty group. Reuse it.
    if (Group* ghost = findGhostCreator(src))
    {
        HLOGC(cnlog.Debug, log << "SRTLA REG1 retransmit from " << src.str() << " -> resend REG2");
        sendReg2(src, *ghost);
        return;
    }

    // Rule 2: table full — try to evict the oldest ghost group.
    if (m_Groups.size() >= MAX_GROUPS)
    {
        std::list<Group>::iterator victim = m_Groups.end();
        for (std::list<Group>::iterator g = m_Groups.begin(); g != m_Groups.end(); ++g)
        {
            if (g->links.empty() && !g->data_seen)
            {
                if (victim == m_Groups.end() || g->created_at < victim->created_at)
                    victim = g;
            }
        }
        if (victim == m_Groups.end())
        {
            HLOGC(cnlog.Debug, log << "SRTLA REG1: group table full, no ghost to evict -> REG_ERR");
            sendShort(src, T_REG_ERR);
            return;
        }
        removeGroup(victim);
    }

    // Rule 3-5: create the group, mint the server half, reply REG2.
    m_Groups.push_back(Group());
    Group& g = m_Groups.back();
    memcpy(g.id, wire + 2, SRTLA_ID_HALF);              // client half [0..127]
    fillRandom(g.id + SRTLA_ID_HALF, SRTLA_ID_HALF);    // server half [128..255]
    g.created_at    = steady_clock::now();
    g.last_addr     = src;
    g.has_last_addr = true;

    HLOGC(cnlog.Debug, log << "SRTLA REG1 from " << src.str() << " -> new group, REG2 (groups=" << m_Groups.size() << ")");
    sendReg2(src, g);
}

void srt::SrtlaRec::handleReg2(const sockaddr_any& src, const uint8_t* wire)
{
    const uint8_t* full_id = wire + 2;

    Group* g = findGroupById(full_id);
    if (!g)
    {
        HLOGC(cnlog.Debug, log << "SRTLA REG2 from " << src.str() << ": unknown group -> REG_NGP");
        sendShort(src, T_REG_NGP);
        return;
    }

    // If this address already belongs to a different group, reject.
    LinkRef existing = findLink(src);
    if (existing.valid() && existing.g != g)
    {
        HLOGC(cnlog.Debug, log << "SRTLA REG2 from " << src.str() << ": addr in other group -> REG_ERR");
        sendShort(src, T_REG_ERR);
        return;
    }

    // Idempotent: already a link in this group -> just re-ack.
    if (existing.valid() && existing.g == g)
    {
        sendReg3(src);
        return;
    }

    if (g->links.size() >= MAX_CONNS_PER_GROUP)
    {
        HLOGC(cnlog.Debug, log << "SRTLA REG2 from " << src.str() << ": group full -> REG_ERR");
        sendShort(src, T_REG_ERR);
        return;
    }

    // Create the link.
    const time_point now = steady_clock::now();
    g->links.push_back(Link());
    Link& l         = g->links.back();
    l.addr          = src;
    l.last_rcvd     = now;
    l.established   = now;
    l.last_keepalive = now;
    l.connectionId  = fnv1a(src);
    g->last_addr     = src;
    g->has_last_addr = true;

    m_LinkMap[src] = LinkRef(g, &l);

    HLOGC(cnlog.Debug, log << "SRTLA REG2 from " << src.str() << " -> link joined (links=" << g->links.size()
                           << ") REG3");
    sendReg3(src);
}

// --------------------------------------------------------------------------
// Accounting helpers (spec §4.5)
// --------------------------------------------------------------------------

bool srt::SrtlaRec::trackSn(Group& g, int32_t sn)
{
    std::vector<bool>& win = g.sn_window;
    if (!g.sn_valid)
    {
        g.sn_valid = true;
        g.sn_max   = sn;
        std::fill(win.begin(), win.end(), false);
        win[sn & SN_WINDOW_MASK] = true;
        return true;
    }

    const int32_t diff = (sn - g.sn_max) & 0x7FFFFFFF;
    if (diff == 0)
        return false; // duplicate of current max

    if (diff < 0x40000000)
    {
        // sn is ahead of the window base: advance, clearing vacated slots.
        if (diff >= SN_WINDOW_SIZE)
        {
            std::fill(win.begin(), win.end(), false);
        }
        else
        {
            for (int32_t k = 1; k <= diff; ++k)
                win[(g.sn_max + k) & SN_WINDOW_MASK] = false;
        }
        g.sn_max                 = sn;
        win[sn & SN_WINDOW_MASK] = true;
        return true;
    }

    // sn is behind the base (older / retransmission).
    const uint32_t back = uint32_t((g.sn_max - sn) & 0x7FFFFFFF);
    if (back >= uint32_t(SN_WINDOW_SIZE))
        return false; // too old to tell — treat as retransmission
    if (win[sn & SN_WINDOW_MASK])
        return false; // already seen
    win[sn & SN_WINDOW_MASK] = true;
    return true;
}

// --------------------------------------------------------------------------
// Ingress (spec §4.3)
// --------------------------------------------------------------------------

srt::SrtlaRec::Ingress srt::SrtlaRec::onIngress(const sockaddr_any& src, CUnit* unit)
{
    CPacket&       pkt   = unit->m_Packet;
    const uint16_t T     = peekType(pkt);
    const size_t   plen  = pkt.getLength();
    // The channel computed the payload length as (recv_size - 16), which underflows
    // for a datagram shorter than the 16-byte SRT header. Detect that so a tiny
    // control packet (e.g. a 2-byte keepalive) is not mistaken for a giant one. The
    // 16-byte header buffer is always present, so the type is still readable.
    const bool     valid_len = plen <= 65535;
    const size_t   total     = valid_len ? plen + 16 : 0;

    ScopedLock lock(m_Lock);
    const time_point now = steady_clock::now();

    // 1. Registration (never reaches a CUDT).
    if (T == T_REG1)
    {
        if (total == REG_LEN)
        {
            uint8_t wire[REG_LEN];
            if (rebuildWire(pkt, wire, sizeof wire) == REG_LEN)
                handleReg1(src, wire);
        }
        return SRTLA_DROP;
    }
    if (T == T_REG2)
    {
        if (total == REG_LEN)
        {
            uint8_t wire[REG_LEN];
            if (rebuildWire(pkt, wire, sizeof wire) == REG_LEN)
                handleReg2(src, wire);
        }
        return SRTLA_DROP;
    }

    // 2. Membership: only registered links may send non-registration traffic.
    LinkRef ref = findLink(src);
    if (!ref.valid())
        return SRTLA_DROP;
    Group* g = ref.g;
    Link*  l = ref.l;

    // 3. Liveness / recovery.
    if (count_milliseconds(now - l->last_rcvd) > 4000 && !l->recovering)
    {
        l->recovering     = true;
        l->recovery_start = now;
        l->last_keepalive = now;
    }
    l->last_rcvd = now;

    // Decay the rate accumulators to 'now', then add this packet's wire bytes. unique_acc
    // is decayed here and topped up for new data packets below.
    if (valid_len)
    {
        double decay = 0.0;
        if (l->rate_init)
        {
            const double dt = double(count_microseconds(now - l->rate_last)) * EWMA_INV_TAU;
            decay = dt >= 1.0 ? 0.0 : 1.0 - dt;
        }
        l->rate_init  = true;
        l->recv_acc   = l->recv_acc * decay + double(total);
        l->unique_acc = l->unique_acc * decay;
        l->rate_last  = now;
    }

    // 4. Keepalive: echo verbatim.
    if (T == T_KEEPALIVE)
    {
        echoKeepalive(src, pkt);
        return SRTLA_DROP;
    }

    // Any other SRTLA-native control (0x9xxx / 0xAxxx) is never SRT: consume it.
    if (T >= 0x9000)
        return SRTLA_DROP;

    // 5. SRT traffic (data or SRT control) from a registered link.
    if (!valid_len || total < SRT_MIN_LEN)
        return SRTLA_DROP;

    g->last_addr     = src;
    g->has_last_addr = true;
    g->data_seen     = true;

    const uint32_t w0 = pkt.getHeader()[SRT_PH_SEQNO];
    if ((w0 & 0x80000000u) == 0) // SRT data packet
    {
        // Count first-seen (group-wide) sequence numbers as unique. First-seen tracking,
        // not the rexmit flag: a retransmit the receiver never got is still new.
        const int32_t sn     = int32_t(w0 & 0x7FFFFFFF);
        const bool    is_new = trackSn(*g, sn);
        if (is_new)
        {
            l->unique_acc += double(total); // wire bytes, same basis as recv_acc (decayed above)

            // Relative one-way transit (new packets only). rel = (arrival - arrival_ref)
            // - (sender_ts - ts_ref); the sender clock offset cancels. Sender timestamp
            // unwrapped with signed 32-bit deltas (wrap- and reorder-safe).
            const uint32_t sender_ts = pkt.getHeader()[SRT_PH_TIMESTAMP];
            if (!g->ts_init)
            {
                g->ts_init       = true;
                g->ts_last_raw   = sender_ts;
                g->sender_ts_acc = 0;
                g->arrival_ref   = now;
            }
            else
            {
                g->sender_ts_acc += int32_t(sender_ts - g->ts_last_raw);
                g->ts_last_raw    = sender_ts;
            }
            const int64_t rel = count_microseconds(now - g->arrival_ref) - g->sender_ts_acc;

            // Transit EWMA (time-weighted) → RTT anchor. Jitter EWMA (RFC 3550).
            if (!l->transit_valid)
            {
                l->transit_ewma  = double(rel);
                l->transit_valid = true;
            }
            else
            {
                double a = double(count_microseconds(now - l->last_transit)) * EWMA_INV_TAU;
                if (a > 1.0)
                    a = 1.0;
                l->transit_ewma += (double(rel) - l->transit_ewma) * a;
            }
            l->last_transit = now;

            if (l->have_prev_transit)
            {
                const int64_t d  = rel - l->prev_transit;
                const double  ad = double(d < 0 ? -d : d);
                l->jitter_ewma += (ad - l->jitter_ewma) / 16.0; // RFC 3550
            }
            l->prev_transit      = rel;
            l->have_prev_transit = true;
        }

        if (l->ack_count == 0)
            l->ack_first_pending = now;
        l->ack_log[l->ack_count++] = uint32_t(sn);
        // Flush on a full batch or when the oldest pending entry has aged out.
        if (l->ack_count >= RECV_ACK_INT
                || count_microseconds(now - l->ack_first_pending) >= ACK_FLUSH_US)
        {
            sendSrtlaAck(src, *l);
        }
    }

    return SRTLA_PASS; // let the worker dispatch it to the bound CUDT
}

// --------------------------------------------------------------------------
// Egress (spec §4.6 / §5)
// --------------------------------------------------------------------------

bool srt::SrtlaRec::onEgress(const sockaddr_any& peer, CPacket& pkt, const sockaddr_any& src)
{
    ScopedLock lock(m_Lock);

    Group* g = findGroupByEgress(peer);
    if (!g)
        return false; // not an SRTLA group destination: caller sends normally

    if (pkt.isControl())
    {
        const UDTMessageType mt = pkt.getType();
        if (mt == UMSG_ACK || mt == UMSG_LOSSREPORT)
        {
            // Fan SRT ACK / NAK out to every link so the sender can balance and
            // recover on all paths. sendto() round-trips byte order per call.
            for (std::list<Link>::iterator it = g->links.begin(); it != g->links.end(); ++it)
                m_pChannel->sendto(it->addr, pkt, src);
            if (g->links.empty())
                m_pChannel->sendto(peer, pkt, src);
            return true;
        }
    }

    // Everything else goes to the group's most recently active link.
    const sockaddr_any& dst = g->has_last_addr ? g->last_addr : peer;
    m_pChannel->sendto(dst, pkt, src);
    return true;
}

// --------------------------------------------------------------------------
// Binding (spec §4.4)
// --------------------------------------------------------------------------

void srt::SrtlaRec::bindGroup(const sockaddr_any& peer, CUDT* u)
{
    ScopedLock lock(m_Lock);

    LinkRef ref = findLink(peer);
    if (!ref.valid())
    {
        // The connecting address is not a registered SRTLA link; leave it to the
        // normal SRT path (should not happen for an SRTLA listener).
        HLOGC(cnlog.Debug, log << "SRTLA bindGroup: peer " << peer.str() << " is not a registered link");
        return;
    }

    Group* g       = ref.g;
    g->socket_id   = u->socketID();
    g->bound       = true;
    if (!g->has_last_addr)
    {
        g->last_addr     = peer;
        g->has_last_addr = true;
    }
    m_EgressMap[peer] = g;

    HLOGC(cnlog.Debug, log << "SRTLA bindGroup: peer " << peer.str() << " -> CUDT @" << u->socketID());
}

// --------------------------------------------------------------------------
// Periodic: timeouts, recovery keepalives, per-second stats (spec §6, §3.6)
// --------------------------------------------------------------------------

void srt::SrtlaRec::onPeriodic(const time_point& now)
{
    ScopedLock lock(m_Lock);

    if (!m_HaveTimers)
    {
        m_LastCleanup = now;
        m_HaveTimers  = true;
        return;
    }

    // Flush aged partial SRTLA-ACK batches (covers links that went quiet).
    for (std::list<Group>::iterator g = m_Groups.begin(); g != m_Groups.end(); ++g)
    {
        for (std::list<Link>::iterator it = g->links.begin(); it != g->links.end(); ++it)
        {
            if (it->ack_count > 0 && count_microseconds(now - it->ack_first_pending) >= ACK_FLUSH_US)
                sendSrtlaAck(it->addr, *it);
        }
    }

    if (count_milliseconds(now - m_LastCleanup) >= 3000) // CLEANUP_PERIOD
    {
        doCleanup(now);
        m_LastCleanup = now;
    }
}

void srt::SrtlaRec::removeGroup(std::list<Group>::iterator git)
{
    for (std::list<Link>::iterator it = git->links.begin(); it != git->links.end(); ++it)
        m_LinkMap.erase(it->addr);

    // Erase every egress mapping that points at this group.
    for (std::map<sockaddr_any, Group*, AddrLess>::iterator e = m_EgressMap.begin();
         e != m_EgressMap.end();)
    {
        if (e->second == &*git)
            m_EgressMap.erase(e++);
        else
            ++e;
    }

    m_Groups.erase(git);
}

void srt::SrtlaRec::doCleanup(const time_point& now)
{
    for (std::list<Group>::iterator g = m_Groups.begin(); g != m_Groups.end();)
    {
        // Drop links idle for more than CONN_TIMEOUT (4 s).
        for (std::list<Link>::iterator it = g->links.begin(); it != g->links.end();)
        {
            if (count_milliseconds(now - it->last_rcvd) > 4000)
            {
                HLOGC(cnlog.Debug, log << "SRTLA link timeout " << it->addr.str());
                m_LinkMap.erase(it->addr);
                it = g->links.erase(it);
            }
            else
            {
                // Recovery: send more frequent keepalives; complete after 5 s healthy.
                if (it->recovering)
                {
                    if (count_milliseconds(now - it->recovery_start) >= 5000) // RECOVERY_CHANCE_PERIOD
                    {
                        it->recovering = false;
                    }
                    else if (count_milliseconds(now - it->last_keepalive) >= 1000) // KEEPALIVE_PERIOD
                    {
                        sendKeepalive(it->addr);
                        it->last_keepalive = now;
                    }
                }
                ++it;
            }
        }

        // Remove empty groups past the initial grace period (GROUP_TIMEOUT 4 s).
        if (g->links.empty() && count_milliseconds(now - g->created_at) > 4000)
        {
            HLOGC(cnlog.Debug, log << "SRTLA group timeout (empty)");
            std::list<Group>::iterator dead = g++;
            removeGroup(dead);
        }
        else
        {
            ++g;
        }
    }
}

uint32_t srt::SrtlaRec::holdSteadyUs(SRTSOCKET socket_id)
{
    ScopedLock lock(m_Lock);

    Group* grp = NULL;
    for (std::list<Group>::iterator g = m_Groups.begin(); g != m_Groups.end(); ++g)
    {
        if (g->bound && g->socket_id == socket_id)
        {
            grp = &*g;
            break;
        }
    }
    if (!grp)
        return 0;

    double tmin = 0.0, tmax = 0.0, jmax = 0.0;
    bool   have = false;
    for (std::list<Link>::iterator it = grp->links.begin(); it != grp->links.end(); ++it)
    {
        if (!it->transit_valid)
            continue;
        if (!have)
        {
            tmin = tmax = it->transit_ewma;
            have = true;
        }
        else
        {
            if (it->transit_ewma < tmin)
                tmin = it->transit_ewma;
            if (it->transit_ewma > tmax)
                tmax = it->transit_ewma;
        }
        if (it->jitter_ewma > jmax)
            jmax = it->jitter_ewma;
    }
    if (!have)
        return 0;

    double hold = (tmax - tmin) + 4.0 * jmax;
    if (hold < 0.0)
        hold = 0.0;
    if (hold > 10e6)
        hold = 10e6;
    return (uint32_t)hold;
}

// Fill @a out with the group's live per-link EWMA values (read fresh); rates get a
// read-time decay so they are current at this instant.
bool srt::SrtlaRec::fillStats(SRTSOCKET socket_id, SRT_SRTLA_STATS* out)
{
    ScopedLock lock(m_Lock);

    memset(out, 0, sizeof *out);
    out->version = 2;

    Group* grp = NULL;
    for (std::list<Group>::iterator g = m_Groups.begin(); g != m_Groups.end(); ++g)
    {
        if (g->bound && g->socket_id == socket_id)
        {
            grp = &*g;
            break;
        }
    }
    if (!grp)
        return false;

    const time_point now = steady_clock::now();
    out->valid = 1;
    out->nowMs = uint64_t(count_milliseconds(now.time_since_epoch()));

    int i = 0;
    for (std::list<Link>::iterator it = grp->links.begin();
         it != grp->links.end() && i < SRT_SRTLA_MAX_PEERS; ++it, ++i)
    {
        double decay = 0.0;
        if (it->rate_init)
        {
            const double dt = double(count_microseconds(now - it->rate_last)) * EWMA_INV_TAU;
            decay = dt >= 1.0 ? 0.0 : 1.0 - dt;
        }
        out->peers[i].connectionId   = it->connectionId;
        out->peers[i].kbpsRecvRate   = uint32_t(it->recv_acc   * decay * 8.0 / 1000.0);
        out->peers[i].kbpsRecvUnique = uint32_t(it->unique_acc * decay * 8.0 / 1000.0);
        out->peers[i].transitUs      = int32_t(it->transit_ewma);
        out->peers[i].usJitter       = uint32_t(it->jitter_ewma);
        out->peers[i].establishedMs  = uint64_t(count_milliseconds(it->established.time_since_epoch()));
    }
    out->numPeers = uint8_t(i);
    return true;
}
