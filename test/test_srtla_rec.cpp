/*
 * SRTLA receiver demux regression tests.
 * Copyright (c) 2026 OpenIRL
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Covers the "keep a group with its connection and reset its accounting on
 * rebind" fixes: a group bound to a live SRT connection survives an outage of
 * all its links, a new connection on a surviving group starts fresh accounting,
 * packets of another socket do not touch it, and a terminated connection
 * releases its group.
 */

#include <cstdint>
#include <cstring>

#include "gtest/gtest.h"
#include "test_env.h"

#include "channel.h"
#include "core.h"
#include "packet.h"
#include "queue.h"
#include "srtla_rec.h"

using namespace srt;

namespace srt
{

/// Test-only access to the demux's private registry and accounting helpers.
/// Declared a friend by SrtlaRec so the per-link / per-group state can be
/// inspected and aged deterministically, without waiting for real timeouts.
class SrtlaRecTestAccess
{
public:
    typedef SrtlaRec::Group     Group;
    typedef SrtlaRec::Link      Link;
    typedef SrtlaRec::time_point time_point;

    /// Create a group through the real REG1 path and return its full (client +
    /// server half) id.
    static void reg1(SrtlaRec& rec, const sockaddr_any& src, uint8_t seed, uint8_t* full_id)
    {
        uint8_t wire[SrtlaRec::REG_LEN];
        memset(wire, 0, sizeof wire);
        wire[0] = 0x92;
        wire[1] = 0x00;
        memset(wire + 2, seed, SrtlaRec::SRTLA_ID_HALF);

        rec.handleReg1(src, wire);

        EXPECT_FALSE(rec.m_Groups.empty());
        Group& g = rec.m_Groups.back();
        memcpy(full_id, g.id, SrtlaRec::SRTLA_ID_LEN);
    }

    /// Register another link through the real REG2 path.
    static void reg2(SrtlaRec& rec, const sockaddr_any& src, const uint8_t* full_id)
    {
        uint8_t wire[SrtlaRec::REG_LEN];
        memset(wire, 0, sizeof wire);
        wire[0] = 0x92;
        wire[1] = 0x01;
        memcpy(wire + 2, full_id, SrtlaRec::SRTLA_ID_LEN);

        rec.handleReg2(src, wire);
    }

    static Group* groupById(SrtlaRec& rec, const uint8_t* full_id) { return rec.findGroupById(full_id); }

    static size_t groupCount(const SrtlaRec& rec) { return rec.m_Groups.size(); }

    static size_t linkCount(const Group& g) { return g.links.size(); }

    static Link* linkByAddr(Group& g, const sockaddr_any& addr)
    {
        for (std::list<Link>::iterator it = g.links.begin(); it != g.links.end(); ++it)
        {
            if (it->addr == addr)
                return &*it;
        }
        return nullptr;
    }

    static void ageLink(Group& g, const sockaddr_any& addr, const time_point& last_rcvd)
    {
        Link* l = linkByAddr(g, addr);
        ASSERT_NE(l, nullptr);
        l->last_rcvd = last_rcvd;
    }

    static void ageGroup(Group& g, const time_point& created_at) { g.created_at = created_at; }

    static void cleanup(SrtlaRec& rec, const time_point& now) { rec.doCleanup(now); }

    static bool snTrackingValid(const Group& g) { return g.sn_valid; }
    static int32_t snMax(const Group& g) { return g.sn_max; }
    static bool timestampValid(const Group& g) { return g.ts_init; }
    static int64_t senderTimeAcc(const Group& g) { return g.sender_ts_acc; }

    static double uniqueBytes(const Link& l) { return l.unique_acc; }
    static double transitUs(const Link& l) { return l.transit_ewma; }
    static double absTransitUs(const Link& l) { return l.transit_ewma < 0 ? -l.transit_ewma : l.transit_ewma; }
    static bool transitValid(const Link& l) { return l.transit_valid; }
    static double jitterUs(const Link& l) { return l.jitter_ewma; }
    static bool havePrevTransit(const Link& l) { return l.have_prev_transit; }
};

} // namespace srt

namespace
{

sockaddr_any LinkAddr(unsigned short port)
{
    return CreateAddr("127.0.0.1", port, AF_INET);
}

int64_t iabs(int64_t v)
{
    return v < 0 ? -v : v;
}

class SrtlaRecTest : public ::testing::Test
{
protected:
    TestInit m_init;
    CChannel m_chan;
    SrtlaRec* m_rec;

    void SetUp() override
    {
        m_chan.open(AF_INET);
        m_rec = new SrtlaRec(&m_chan);
    }

    void TearDown() override
    {
        delete m_rec;
        m_rec = nullptr;
        m_chan.close();
    }

    /// Deliver one SRT data packet as the channel would after byte-order
    /// conversion: the header is in host order and the source address is the
    /// registered link.
    void feedData(const sockaddr_any& src, int32_t sn, uint32_t timestamp, SRTSOCKET dest,
                  size_t payload = 1000, bool rexmit = false)
    {
        CUnit unit;
        unit.m_Packet.setLength(payload);
        uint32_t* h = unit.m_Packet.getHeader();
        h[SRT_PH_SEQNO]     = uint32_t(sn);
        h[SRT_PH_TIMESTAMP] = timestamp;
        h[SRT_PH_MSGNO]     = rexmit ? MSGNO_REXMIT::mask : 0;
        h[SRT_PH_ID]        = int32_t(dest);

        EXPECT_EQ(m_rec->onIngress(src, &unit), SrtlaRec::SRTLA_PASS);
    }

    CUDT* coreOf(SRTSOCKET s)
    {
        TestMockCUDT mock;
        return mock.setSocket(s) ? mock.core : nullptr;
    }
};

// All links of a group drop while its SRT connection stays alive: the group
// must survive cleanup (no REG_NGP on re-registration), accept the links back
// under the same id, and only go away once the connection is unbound.
TEST_F(SrtlaRecTest, GroupSurvivesLinkOutageWhileBound)
{
    const sockaddr_any link1 = LinkAddr(45001);
    const sockaddr_any link2 = LinkAddr(45002);
    uint8_t group_id[SrtlaRec::SRTLA_ID_LEN];
    SrtlaRecTestAccess::reg1(*m_rec, link1, 0xA1, group_id);
    SrtlaRecTestAccess::reg2(*m_rec, link1, group_id);
    SrtlaRecTestAccess::reg2(*m_rec, link2, group_id);

    ASSERT_EQ(SrtlaRecTestAccess::groupCount(*m_rec), 1u);
    SrtlaRecTestAccess::Group* g = SrtlaRecTestAccess::groupById(*m_rec, group_id);
    ASSERT_NE(g, nullptr);
    ASSERT_EQ(SrtlaRecTestAccess::linkCount(*g), 2u);

    SRTSOCKET sock = srt_create_socket();
    ASSERT_NE(sock, SRT_INVALID_SOCK);
    CUDT* core = coreOf(sock);
    ASSERT_NE(core, nullptr);
    m_rec->bindGroup(link1, core);
    ASSERT_TRUE(g->bound);
    ASSERT_EQ(g->socket_id, sock);

    // Every link goes silent for longer than CONN_TIMEOUT, while the SRT
    // connection is still up.
    const sync::steady_clock::time_point now = sync::steady_clock::now();
    SrtlaRecTestAccess::ageLink(*g, link1, now - sync::milliseconds_from(10000));
    SrtlaRecTestAccess::ageLink(*g, link2, now - sync::milliseconds_from(10000));
    SrtlaRecTestAccess::cleanup(*m_rec, now);

    ASSERT_EQ(SrtlaRecTestAccess::linkCount(*g), 0u);
    ASSERT_EQ(SrtlaRecTestAccess::groupCount(*m_rec), 1u) << "a bound group must survive an empty link set";

    // The sender re-registers with REG2 and the same group id: it must find the
    // group and become a link again.
    SrtlaRecTestAccess::reg2(*m_rec, link1, group_id);
    EXPECT_EQ(SrtlaRecTestAccess::groupCount(*m_rec), 1u);
    SrtlaRecTestAccess::Group* still = SrtlaRecTestAccess::groupById(*m_rec, group_id);
    ASSERT_NE(still, nullptr);
    EXPECT_EQ(still, g) << "re-registration must reuse the existing group";
    EXPECT_EQ(SrtlaRecTestAccess::linkCount(*still), 1u);
    EXPECT_TRUE(still->bound);
    EXPECT_EQ(still->socket_id, sock);

    // Once the connection leaves the receive queue, the empty group times out.
    m_rec->unbindSocket(sock);
    EXPECT_FALSE(g->bound);
    EXPECT_EQ(g->socket_id, SRT_INVALID_SOCK);
    SrtlaRecTestAccess::ageGroup(*g, now - sync::milliseconds_from(10000));
    const sync::steady_clock::time_point later = now + sync::milliseconds_from(5000);
    SrtlaRecTestAccess::ageLink(*g, link1, later - sync::milliseconds_from(10000));
    SrtlaRecTestAccess::cleanup(*m_rec, later);
    EXPECT_EQ(SrtlaRecTestAccess::groupCount(*m_rec), 0u);

    srt_close(sock);
}

// A group without links and without a connection keeps the original cleanup
// behaviour: it is removed past the initial grace period.
TEST_F(SrtlaRecTest, UnboundEmptyGroupTimesOut)
{
    const sockaddr_any link = LinkAddr(45003);
    uint8_t group_id[SrtlaRec::SRTLA_ID_LEN];
    SrtlaRecTestAccess::reg1(*m_rec, link, 0xB2, group_id);

    ASSERT_EQ(SrtlaRecTestAccess::groupCount(*m_rec), 1u);
    SrtlaRecTestAccess::Group* g = SrtlaRecTestAccess::groupById(*m_rec, group_id);
    ASSERT_NE(g, nullptr);

    const sync::steady_clock::time_point now = sync::steady_clock::now();
    SrtlaRecTestAccess::ageGroup(*g, now - sync::milliseconds_from(10000));
    SrtlaRecTestAccess::cleanup(*m_rec, now);
    EXPECT_EQ(SrtlaRecTestAccess::groupCount(*m_rec), 0u);
}

// The sender's SRT reconnects while the SRTLA links stay registered. The new
// socket must start a fresh sequence-number / timestamp / rate / transit /
// jitter state, and only packets addressed to it may update that state.
TEST_F(SrtlaRecTest, RebindResetsAccountingAndIgnoresOldConnection)
{
    const sockaddr_any link = LinkAddr(45004);
    uint8_t group_id[SrtlaRec::SRTLA_ID_LEN];
    SrtlaRecTestAccess::reg1(*m_rec, link, 0xC3, group_id);
    SrtlaRecTestAccess::reg2(*m_rec, link, group_id);
    SrtlaRecTestAccess::Group* g = SrtlaRecTestAccess::groupById(*m_rec, group_id);
    ASSERT_NE(g, nullptr);

    SRTSOCKET old_sock = srt_create_socket();
    SRTSOCKET new_sock = srt_create_socket();
    ASSERT_NE(old_sock, SRT_INVALID_SOCK);
    ASSERT_NE(new_sock, SRT_INVALID_SOCK);
    CUDT* old_core = coreOf(old_sock);
    CUDT* new_core = coreOf(new_sock);
    ASSERT_NE(old_core, nullptr);
    ASSERT_NE(new_core, nullptr);

    m_rec->bindGroup(link, old_core);
    ASSERT_TRUE(g->bound);
    ASSERT_EQ(g->socket_id, old_sock);

    // Old connection traffic establishes a sequence-number window and a transit
    // reference that, if kept, would be far outside the new connection's range.
    feedData(link, 1000, 0, old_sock);
    feedData(link, 1001, 1000000, old_sock);
    SrtlaRecTestAccess::Link* l = SrtlaRecTestAccess::linkByAddr(*g, link);
    ASSERT_NE(l, nullptr);
    EXPECT_TRUE(SrtlaRecTestAccess::snTrackingValid(*g));
    EXPECT_EQ(SrtlaRecTestAccess::snMax(*g), 1001);
    EXPECT_TRUE(SrtlaRecTestAccess::timestampValid(*g));
    EXPECT_TRUE(SrtlaRecTestAccess::transitValid(*l));
    EXPECT_GT(SrtlaRecTestAccess::uniqueBytes(*l), 0.0);
    EXPECT_TRUE(SrtlaRecTestAccess::havePrevTransit(*l));

    // Reconnect: same group, different CUDT.
    m_rec->bindGroup(link, new_core);
    ASSERT_TRUE(g->bound);
    ASSERT_EQ(g->socket_id, new_sock);

    EXPECT_FALSE(SrtlaRecTestAccess::snTrackingValid(*g));
    EXPECT_FALSE(SrtlaRecTestAccess::timestampValid(*g));
    EXPECT_EQ(SrtlaRecTestAccess::uniqueBytes(*l), 0.0);
    EXPECT_FALSE(SrtlaRecTestAccess::transitValid(*l));
    EXPECT_EQ(SrtlaRecTestAccess::transitUs(*l), 0.0);
    EXPECT_EQ(SrtlaRecTestAccess::jitterUs(*l), 0.0);
    EXPECT_FALSE(SrtlaRecTestAccess::havePrevTransit(*l));
    EXPECT_EQ(SrtlaRecTestAccess::senderTimeAcc(*g), 0u);

    // A delayed packet of the old connection (addressed to the old socket) and
    // a refused reconnect attempt must not reopen or advance the window.
    feedData(link, 5000, 2000000, old_sock);
    EXPECT_FALSE(SrtlaRecTestAccess::snTrackingValid(*g));
    EXPECT_FALSE(SrtlaRecTestAccess::timestampValid(*g));
    EXPECT_FALSE(SrtlaRecTestAccess::transitValid(*l));
    EXPECT_EQ(SrtlaRecTestAccess::uniqueBytes(*l), 0.0);

    // The new connection's first packet starts in its own space. A large sender
    // timestamp (a restarted sender clock) must not produce a huge relative
    // transit from the old arrival reference.
    feedData(link, 7, 3000000000u, new_sock);
    EXPECT_TRUE(SrtlaRecTestAccess::snTrackingValid(*g));
    EXPECT_EQ(SrtlaRecTestAccess::snMax(*g), 7) << "a stale packet must not move the window base";
    EXPECT_TRUE(SrtlaRecTestAccess::timestampValid(*g));
    EXPECT_GT(SrtlaRecTestAccess::uniqueBytes(*l), 0.0);
    EXPECT_TRUE(SrtlaRecTestAccess::transitValid(*l));
    EXPECT_LT(SrtlaRecTestAccess::absTransitUs(*l), 1000000.0)
        << "transit must be relative to the new connection";

    // Jitter is measured again from the new connection's packets: a varying
    // sender timestamp must produce a non-zero RFC 3550 estimate.
    feedData(link, 8, 2999995000u, new_sock);
    EXPECT_GT(SrtlaRecTestAccess::jitterUs(*l), 0.0);
    EXPECT_LT(SrtlaRecTestAccess::absTransitUs(*l), 1000000.0);

    srt_close(old_sock);
    srt_close(new_sock);
}

// Stats queried for a socket follow the current binding only: after a rebind the
// old socket reports nothing and the new one reports the new connection's
// per-link bitrate / transit / jitter.
TEST_F(SrtlaRecTest, StatsFollowRebind)
{
    const sockaddr_any link = LinkAddr(45005);
    uint8_t group_id[SrtlaRec::SRTLA_ID_LEN];
    SrtlaRecTestAccess::reg1(*m_rec, link, 0xD4, group_id);
    SrtlaRecTestAccess::reg2(*m_rec, link, group_id);
    SrtlaRecTestAccess::Group* g = SrtlaRecTestAccess::groupById(*m_rec, group_id);
    ASSERT_NE(g, nullptr);

    SRTSOCKET old_sock = srt_create_socket();
    SRTSOCKET new_sock = srt_create_socket();
    ASSERT_NE(old_sock, SRT_INVALID_SOCK);
    ASSERT_NE(new_sock, SRT_INVALID_SOCK);
    CUDT* old_core = coreOf(old_sock);
    CUDT* new_core = coreOf(new_sock);
    ASSERT_NE(old_core, nullptr);
    ASSERT_NE(new_core, nullptr);

    m_rec->bindGroup(link, old_core);
    for (int32_t i = 0; i < 5; ++i)
        feedData(link, 100 + i, uint32_t(i) * 1000, old_sock);

    SRT_SRTLA_STATS stats;
    ASSERT_TRUE(m_rec->fillStats(old_sock, &stats));
    EXPECT_EQ(stats.valid, 1);
    ASSERT_EQ(stats.numPeers, 1);
    EXPECT_GT(stats.peers[0].kbpsRecvUnique, 0u);

    m_rec->bindGroup(link, new_core);

    // The previous connection's statistics must not be attributed to the new
    // one before it has sent anything.
    ASSERT_TRUE(m_rec->fillStats(new_sock, &stats));
    EXPECT_EQ(stats.valid, 1);
    ASSERT_EQ(stats.numPeers, 1);
    EXPECT_EQ(stats.peers[0].kbpsRecvUnique, 0u);
    EXPECT_EQ(stats.peers[0].transitUs, 0);
    EXPECT_EQ(stats.peers[0].usJitter, 0u);
    EXPECT_FALSE(m_rec->fillStats(old_sock, &stats));

    for (int32_t i = 0; i < 5; ++i)
        feedData(link, i, uint32_t(i) * 1000, new_sock);

    ASSERT_TRUE(m_rec->fillStats(new_sock, &stats));
    ASSERT_EQ(stats.numPeers, 1);
    EXPECT_GT(stats.peers[0].kbpsRecvUnique, 0u);
    EXPECT_LT(iabs(stats.peers[0].transitUs), 1000000);

    srt_close(old_sock);
    srt_close(new_sock);
}

// Re-binding the same connection must not throw its accounting away: only a
// different socket starts a new measurement space.
TEST_F(SrtlaRecTest, SameSocketRebindKeepsAccounting)
{
    const sockaddr_any link = LinkAddr(45006);
    uint8_t group_id[SrtlaRec::SRTLA_ID_LEN];
    SrtlaRecTestAccess::reg1(*m_rec, link, 0xE5, group_id);
    SrtlaRecTestAccess::reg2(*m_rec, link, group_id);
    SrtlaRecTestAccess::Group* g = SrtlaRecTestAccess::groupById(*m_rec, group_id);
    ASSERT_NE(g, nullptr);

    SRTSOCKET sock = srt_create_socket();
    ASSERT_NE(sock, SRT_INVALID_SOCK);
    CUDT* core = coreOf(sock);
    ASSERT_NE(core, nullptr);

    m_rec->bindGroup(link, core);
    feedData(link, 42, 0, sock);
    SrtlaRecTestAccess::Link* l = SrtlaRecTestAccess::linkByAddr(*g, link);
    ASSERT_NE(l, nullptr);
    EXPECT_TRUE(SrtlaRecTestAccess::snTrackingValid(*g));
    EXPECT_TRUE(SrtlaRecTestAccess::timestampValid(*g));
    EXPECT_GT(SrtlaRecTestAccess::uniqueBytes(*l), 0.0);

    m_rec->bindGroup(link, core);
    EXPECT_TRUE(SrtlaRecTestAccess::snTrackingValid(*g));
    EXPECT_EQ(SrtlaRecTestAccess::snMax(*g), 42);
    EXPECT_TRUE(SrtlaRecTestAccess::timestampValid(*g));
    EXPECT_GT(SrtlaRecTestAccess::uniqueBytes(*l), 0.0);

    srt_close(sock);
}

// Unbinding one connection must not disturb a group bound to another socket.
TEST_F(SrtlaRecTest, UnbindReleasesOnlyMatchingGroup)
{
    const sockaddr_any link_a = LinkAddr(45007);
    const sockaddr_any link_b = LinkAddr(45008);
    uint8_t id_a[SrtlaRec::SRTLA_ID_LEN];
    uint8_t id_b[SrtlaRec::SRTLA_ID_LEN];
    SrtlaRecTestAccess::reg1(*m_rec, link_a, 0x11, id_a);
    SrtlaRecTestAccess::reg1(*m_rec, link_b, 0x22, id_b);
    SrtlaRecTestAccess::reg2(*m_rec, link_a, id_a);
    SrtlaRecTestAccess::reg2(*m_rec, link_b, id_b);

    SrtlaRecTestAccess::Group* ga = SrtlaRecTestAccess::groupById(*m_rec, id_a);
    SrtlaRecTestAccess::Group* gb = SrtlaRecTestAccess::groupById(*m_rec, id_b);
    ASSERT_NE(ga, nullptr);
    ASSERT_NE(gb, nullptr);

    SRTSOCKET sock_a = srt_create_socket();
    SRTSOCKET sock_b = srt_create_socket();
    ASSERT_NE(sock_a, SRT_INVALID_SOCK);
    ASSERT_NE(sock_b, SRT_INVALID_SOCK);
    CUDT* core_a = coreOf(sock_a);
    CUDT* core_b = coreOf(sock_b);
    ASSERT_NE(core_a, nullptr);
    ASSERT_NE(core_b, nullptr);

    m_rec->bindGroup(link_a, core_a);
    m_rec->bindGroup(link_b, core_b);

    m_rec->unbindSocket(sock_a);
    EXPECT_FALSE(ga->bound);
    EXPECT_TRUE(gb->bound);
    EXPECT_EQ(gb->socket_id, sock_b);

    // The released group is now eligible for empty-group cleanup; the still
    // bound one is not.
    const sync::steady_clock::time_point now = sync::steady_clock::now();
    SrtlaRecTestAccess::ageGroup(*ga, now - sync::milliseconds_from(10000));
    SrtlaRecTestAccess::ageGroup(*gb, now - sync::milliseconds_from(10000));
    SrtlaRecTestAccess::ageLink(*ga, link_a, now - sync::milliseconds_from(10000));
    SrtlaRecTestAccess::ageLink(*gb, link_b, now - sync::milliseconds_from(10000));
    SrtlaRecTestAccess::cleanup(*m_rec, now);

    EXPECT_EQ(SrtlaRecTestAccess::groupById(*m_rec, id_a), nullptr);
    EXPECT_NE(SrtlaRecTestAccess::groupById(*m_rec, id_b), nullptr);

    srt_close(sock_a);
    srt_close(sock_b);
}

} // namespace
