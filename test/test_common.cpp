#include <stdio.h>
#include <stdlib.h>

#include "gtest/gtest.h"
#include "test_env.h"
#include "utilities.h"
#include "common.h"

using namespace srt;

void test_cipaddress_pton(const char* peer_ip, int family, const uint32_t (&ip)[4])
{
    const int port = 4200;

    // Peer
    sockaddr_storage ss;
    ss.ss_family = family;

    void* sin_addr = nullptr;
    if (family == AF_INET)
    {
        sockaddr_in* const sa = (sockaddr_in*)&ss;
        sa->sin_port          = htons(port);
        sin_addr              = &sa->sin_addr;
    }
    else // IPv6
    {
        sockaddr_in6* const sa = (sockaddr_in6*)&ss;
        sa->sin6_port          = htons(port);
        sin_addr               = &sa->sin6_addr;
    }

    ASSERT_EQ(inet_pton(family, peer_ip, sin_addr), 1);
    const sockaddr_any peer(ss);

    // HOST
    sockaddr_any host(family);
    host.hport(port);

    srt::CIPAddress::pton(host, ip, peer);
    EXPECT_EQ(peer, host) << "Peer " << peer.str() << " host " << host.str();
}

// Example IPv4 address: 192.168.0.1
TEST(CIPAddress, IPv4_pton)
{
    srt::TestInit srtinit;
    const char*    peer_ip = "192.168.0.1";
    const uint32_t ip[4]   = {htobe32(0xC0A80001), 0, 0, 0};
    test_cipaddress_pton(peer_ip, AF_INET, ip);
}

// Example IPv6 address: 2001:db8:85a3:8d3:1319:8a2e:370:7348
TEST(CIPAddress, IPv6_pton)
{
    srt::TestInit srtinit;
    const char*    peer_ip = "2001:db8:85a3:8d3:1319:8a2e:370:7348";
    const uint32_t ip[4]   = {htobe32(0x20010db8), htobe32(0x85a308d3), htobe32(0x13198a2e), htobe32(0x03707348)};

    test_cipaddress_pton(peer_ip, AF_INET6, ip);
}

// Example IPv4 address: 192.168.0.1
// Maps to IPv6 address: 0:0:0:0:0:FFFF:192.168.0.1
// Simplified:                   ::FFFF:192.168.0.1
TEST(CIPAddress, IPv4_in_IPv6_pton)
{
    srt::TestInit srtinit;
    const char*    peer_ip = "::ffff:192.168.0.1";
    const uint32_t ip[4]   = {0, 0, htobe32(0x0000FFFF), htobe32(0xC0A80001)};

    test_cipaddress_pton(peer_ip, AF_INET6, ip);
}

// Quality percentage: the share of a sampling window that was not impaired.
TEST(StatsQuality, Percentage)
{
    // An empty window is reported clean rather than 0/0.
    EXPECT_DOUBLE_EQ(StatsQualityPct(0, 0), 100.0);

    EXPECT_DOUBLE_EQ(StatsQualityPct(100, 0), 100.0);   // nothing impaired
    EXPECT_DOUBLE_EQ(StatsQualityPct(0, 100), 0.0);     // nothing got through
    EXPECT_DOUBLE_EQ(StatsQualityPct(75, 25), 75.0);
    EXPECT_DOUBLE_EQ(StatsQualityPct(1, 1), 50.0);

    // Negative deltas cannot occur (counters are monotonic) but must not blow up.
    EXPECT_DOUBLE_EQ(StatsQualityPct(-5, 0), 100.0);
}

// The metric is a per-window value: each window is scored on its own deltas, so a
// bad second does not drag the following ones down (the failure mode of a
// cumulative counter, which can only ever creep back towards its long-run mean).
TEST(StatsQuality, WindowIsIndependentOfHistory)
{
    // Cumulative counters as they would be sampled at successive window ends.
    struct { int64_t clean, impaired; } snap[] = {
        {   0,   0},   // window start
        {1000,   0},   // w1: clean
        {1500, 500},   // w2: heavily impaired
        {2500, 500},   // w3: clean again
    };

    const double w1 = StatsQualityPct(snap[1].clean - snap[0].clean,
                                      snap[1].impaired - snap[0].impaired);
    const double w2 = StatsQualityPct(snap[2].clean - snap[1].clean,
                                      snap[2].impaired - snap[1].impaired);
    const double w3 = StatsQualityPct(snap[3].clean - snap[2].clean,
                                      snap[3].impaired - snap[2].impaired);

    EXPECT_DOUBLE_EQ(w1, 100.0);
    EXPECT_DOUBLE_EQ(w2, 50.0);    // scored on its own second only
    EXPECT_DOUBLE_EQ(w3, 100.0);   // snaps back, no memory of w2

    // A cumulative reading of the same data would still be dragged down at w3
    // and could only improve from there - the behaviour this replaces.
    const double cumulative_w3 = StatsQualityPct(snap[3].clean, snap[3].impaired);
    EXPECT_LT(cumulative_w3, w3);
}
