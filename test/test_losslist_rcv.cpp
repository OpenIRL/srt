#include <iostream>
#include "gtest/gtest.h"
#include "test_env.h"
#include "common.h"
#include "list.h"

using namespace std;
using namespace srt;

class CRcvLossListTest
    : public ::testing::Test
{
protected:
    void SetUp() override
    {
        m_lossList = new CRcvLossList(CRcvLossListTest::SIZE);
    }

    void TearDown() override
    {
        delete m_lossList;
    }

    void CheckEmptyArray()
    {
        EXPECT_EQ(m_lossList->getLossLength(), 0);
        EXPECT_EQ(m_lossList->getFirstLostSeq(), SRT_SEQNO_NONE);
    }

    void CleanUpList()
    {
        //while (m_lossList->popLostSeq() != -1);
    }

    CRcvLossList* m_lossList;

public:
    const int SIZE = 256;
};

/// Check the state of the freshly created list.
/// Capacity, loss length and pop().
TEST_F(CRcvLossListTest, Create)
{
    CheckEmptyArray();
}

///////////////////////////////////////////////////////////////////////////////
///
/// The first group of tests checks insert and pop()
///
///////////////////////////////////////////////////////////////////////////////

/// Insert and remove one element from the list.
TEST_F(CRcvLossListTest, InsertRemoveOneElem)
{
    EXPECT_EQ(m_lossList->insert(1, 1), 1);

    EXPECT_EQ(m_lossList->getLossLength(), 1);
    EXPECT_TRUE(m_lossList->remove(1, 1));
    CheckEmptyArray();
}


/// Insert and pop one element from the list.
TEST_F(CRcvLossListTest, InsertTwoElemsEdge)
{
    EXPECT_EQ(m_lossList->insert(CSeqNo::m_iMaxSeqNo, 1), 3);
    EXPECT_EQ(m_lossList->getLossLength(), 3);
    EXPECT_TRUE(m_lossList->remove(CSeqNo::m_iMaxSeqNo, 1));
    CheckEmptyArray();
}

TEST(CRcvFreshLossListTest, CheckFreshLossList)
{
    srt::TestInit srtinit;
    std::deque<CRcvFreshLoss> floss {
        CRcvFreshLoss (10, 15, 5),
        CRcvFreshLoss (25, 29, 10),
        CRcvFreshLoss (30, 30, 3),
        CRcvFreshLoss (45, 80, 100)
    };

    EXPECT_EQ(floss.size(), 4u);

    // Ok, now let's do element removal

    int had_ttl = 0;
    bool rm = CRcvFreshLoss::removeOne((floss), 26, &had_ttl);

    EXPECT_EQ(rm, true);
    EXPECT_EQ(had_ttl, 10);
    EXPECT_EQ(floss.size(), 5u);

    // Now we expect to have [10-15] [25-25] [27-35]...
    // After revoking 25 it should have removed it.

    // SPLIT
    rm = CRcvFreshLoss::removeOne((floss), 27, &had_ttl);
    EXPECT_EQ(rm, true);
    EXPECT_EQ(had_ttl, 10);
    EXPECT_EQ(floss.size(), 5u);

    // STRIP
    rm = CRcvFreshLoss::removeOne((floss), 28, &had_ttl);
    EXPECT_EQ(rm, true);
    EXPECT_EQ(had_ttl, 10);
    EXPECT_EQ(floss.size(), 5u);

    // DELETE
    rm = CRcvFreshLoss::removeOne((floss), 25, &had_ttl);
    EXPECT_EQ(rm, true);
    EXPECT_EQ(had_ttl, 10);
    EXPECT_EQ(floss.size(), 4u);

    // SPLIT
    rm = CRcvFreshLoss::removeOne((floss), 50, &had_ttl);
    EXPECT_EQ(rm, true);
    EXPECT_EQ(had_ttl, 100);
    EXPECT_EQ(floss.size(), 5u);

    // DELETE
    rm = CRcvFreshLoss::removeOne((floss), 30, &had_ttl);
    EXPECT_EQ(rm, true);
    EXPECT_EQ(had_ttl, 3);
    EXPECT_EQ(floss.size(), 4u);

    // Remove nonexistent sequence, but existing before.
    rm = CRcvFreshLoss::removeOne((floss), 25, NULL);
    EXPECT_EQ(rm, false);
    EXPECT_EQ(floss.size(), 4u);

    // Remove nonexistent sequence that didn't exist before.
    rm = CRcvFreshLoss::removeOne((floss), 31, &had_ttl);
    EXPECT_EQ(rm, false);
    EXPECT_EQ(had_ttl, 0);
    EXPECT_EQ(floss.size(), 4u);

}

/// A SPLIT must preserve the record's time history in both halves.
TEST(CRcvFreshLossListTest, SplitPreservesTimeHistory)
{
    std::deque<CRcvFreshLoss> floss;
    floss.push_back(CRcvFreshLoss(100, 120, 5));

    // Simulate history: detected some time ago, reported later.
    const srt::sync::steady_clock::time_point detected =
        srt::sync::steady_clock::now() - srt::sync::milliseconds_from(400);
    const srt::sync::steady_clock::time_point reported =
        srt::sync::steady_clock::now() - srt::sync::milliseconds_from(150);
    floss[0].timestamp   = detected;
    floss[0].report_time = reported;

    // SPLIT: sequence in the middle of the range.
    int had_ttl = 0;
    srt::sync::steady_clock::time_point detect_out;
    const bool rm = CRcvFreshLoss::removeOne((floss), 110, &had_ttl, &detect_out);
    ASSERT_TRUE(rm);
    ASSERT_EQ(floss.size(), 2u);

    // The out-parameter reports the ORIGINAL detection time.
    EXPECT_EQ(detect_out, detected);

    // Lower half keeps its history.
    EXPECT_EQ(floss[0].seq[0], 100);
    EXPECT_EQ(floss[0].seq[1], 109);
    EXPECT_EQ(floss[0].timestamp, detected);
    EXPECT_EQ(floss[0].report_time, reported);

    // Upper half is a new object but must carry the same history.
    EXPECT_EQ(floss[1].seq[0], 111);
    EXPECT_EQ(floss[1].seq[1], 120);
    EXPECT_EQ(floss[1].timestamp, detected);
    EXPECT_EQ(floss[1].report_time, reported);
    EXPECT_EQ(floss[1].ttl, floss[0].ttl);
}

/// STRIPPED/DELETE: history survives shrinking; detect-time out-param stays valid.
TEST(CRcvFreshLossListTest, StripAndDeleteReportDetectTime)
{
    std::deque<CRcvFreshLoss> floss;
    floss.push_back(CRcvFreshLoss(200, 202, 3));
    const srt::sync::steady_clock::time_point detected =
        srt::sync::steady_clock::now() - srt::sync::milliseconds_from(300);
    floss[0].timestamp = detected;

    srt::sync::steady_clock::time_point detect_out;

    // STRIPPED (front): range shrinks, history stays.
    ASSERT_TRUE(CRcvFreshLoss::removeOne((floss), 200, NULL, &detect_out));
    EXPECT_EQ(detect_out, detected);
    ASSERT_EQ(floss.size(), 1u);
    EXPECT_EQ(floss[0].seq[0], 201);
    EXPECT_EQ(floss[0].timestamp, detected);

    // STRIPPED (back).
    ASSERT_TRUE(CRcvFreshLoss::removeOne((floss), 202, NULL, &detect_out));
    EXPECT_EQ(detect_out, detected);
    ASSERT_EQ(floss.size(), 1u);
    EXPECT_EQ(floss[0].seq[0], 201);
    EXPECT_EQ(floss[0].seq[1], 201);

    // DELETE (last element): detect time still reported although erased.
    ASSERT_TRUE(CRcvFreshLoss::removeOne((floss), 201, NULL, &detect_out));
    EXPECT_EQ(detect_out, detected);
    EXPECT_TRUE(floss.empty());
}

// --------------------------------------------------------------------------
// srtlaPlanNak: SRTLA loss-report policy
// --------------------------------------------------------------------------

namespace
{
using srt::sync::steady_clock;

/// A record detected @a age_ms ago, never reported, TTL already satisfied.
CRcvFreshLoss makeAged(int32_t lo, int32_t hi, int64_t age_ms)
{
    CRcvFreshLoss rec(lo, hi, 0);
    rec.timestamp = steady_clock::now() - srt::sync::milliseconds_from(age_ms);
    return rec;
}

/// Params for a 1000 ms play budget: 100 ms hold, 200 ms repeat spacing, 50 ms RTT.
srt::SrtlaNakParams makeParams()
{
    srt::SrtlaNakParams p;
    p.hold_us    = 100000;
    p.spacing_us = 200000;
    p.budget_us  = 1000000;
    p.rtt_us     = 50000;
    p.margin_us  = 30000;
    p.cap        = 364; // as for a 1456-byte payload
    return p;
}
} // namespace

/// The reordering hold gates the first report; an aged record passes it.
TEST(SrtlaPlanNak, HoldGatesFirstReport)
{
    std::deque<CRcvFreshLoss> floss;
    floss.push_back(makeAged(10, 12, 50));  // younger than the 100 ms hold
    floss.push_back(makeAged(20, 20, 150)); // past it

    const srt::SrtlaNakPlan plan = srtlaPlanNak(floss, steady_clock::now(), makeParams());

    ASSERT_EQ(plan.report.size(), 1u);
    EXPECT_EQ(plan.report[0], 1u);
    EXPECT_EQ(plan.confirmed, 1); // one packet, first report
    EXPECT_EQ(plan.retire, 0u);
}

/// A record whose TTL has not expired yet is not reported, however old it is.
TEST(SrtlaPlanNak, WitnessTtlBlocksReport)
{
    std::deque<CRcvFreshLoss> floss;
    floss.push_back(makeAged(10, 10, 300));
    floss[0].ttl = 1;

    const srt::SrtlaNakPlan plan = srtlaPlanNak(floss, steady_clock::now(), makeParams());

    EXPECT_TRUE(plan.report.empty());
    EXPECT_EQ(plan.retire, 0u);
}

/// A repeat is held back until the spacing has elapsed.
TEST(SrtlaPlanNak, SpacingGatesRepeat)
{
    const steady_clock::time_point now = steady_clock::now();

    std::deque<CRcvFreshLoss> floss;
    floss.push_back(makeAged(10, 10, 300));
    floss.push_back(makeAged(20, 20, 300));
    floss[0].report_time = now - srt::sync::milliseconds_from(100); // too recent
    floss[1].report_time = now - srt::sync::milliseconds_from(250); // due again

    const srt::SrtlaNakPlan plan = srtlaPlanNak(floss, now, makeParams());

    ASSERT_EQ(plan.report.size(), 1u);
    EXPECT_EQ(plan.report[0], 1u);
    EXPECT_EQ(plan.confirmed, 0); // a repeat is not a newly confirmed loss
}

/// Once no round trip fits in what is left of the play budget, the record is no
/// longer requested.
TEST(SrtlaPlanNak, DeadlineStopsRequesting)
{
    srt::SrtlaNakParams p = makeParams(); // budget 1000 ms, rtt 50 ms, margin 30 ms

    std::deque<CRcvFreshLoss> floss;
    floss.push_back(makeAged(10, 10, 950)); // 950 + 50 + 30 > 1000 -> hopeless
    floss.push_back(makeAged(20, 20, 800)); // 800 + 50 + 30 < 1000 -> still worth it

    const srt::SrtlaNakPlan plan = srtlaPlanNak(floss, steady_clock::now(), p);

    ASSERT_EQ(plan.report.size(), 1u);
    EXPECT_EQ(plan.report[0], 1u);
    EXPECT_EQ(plan.retire, 0u); // not past the budget yet, so not retired either

    // With deadline handling off (no TSBPD) both are requested again.
    p.budget_us = 0;
    const srt::SrtlaNakPlan unbounded = srtlaPlanNak(floss, steady_clock::now(), p);
    EXPECT_EQ(unbounded.report.size(), 2u);
    EXPECT_EQ(unbounded.retire, 0u);
}

/// Records past the play budget form a prefix and are retired, not reported.
TEST(SrtlaPlanNak, RetiresPrefixPastBudget)
{
    std::deque<CRcvFreshLoss> floss;
    floss.push_back(makeAged(10, 10, 3000)); // long dead
    floss.push_back(makeAged(20, 20, 1500)); // dead
    floss.push_back(makeAged(30, 30, 150));  // live and due

    const srt::SrtlaNakPlan plan = srtlaPlanNak(floss, steady_clock::now(), makeParams());

    EXPECT_EQ(plan.retire, 2u);
    ASSERT_EQ(plan.report.size(), 1u);
    EXPECT_EQ(plan.report[0], 2u); // index refers to the untouched container
    EXPECT_EQ(plan.confirmed, 1);
}

/// The report is capped at the payload size, counting 2 words per range and 1 per
/// single sequence. Nothing is reported beyond the cap.
TEST(SrtlaPlanNak, RespectsPayloadCap)
{
    srt::SrtlaNakParams p = makeParams();
    p.cap = 5; // room for two ranges (4 words) plus one single sequence

    std::deque<CRcvFreshLoss> floss;
    floss.push_back(makeAged(10, 12, 150)); // range -> 2 words
    floss.push_back(makeAged(20, 22, 150)); // range -> 2 words
    floss.push_back(makeAged(30, 30, 150)); // single -> 1 word
    floss.push_back(makeAged(40, 40, 150)); // does not fit any more

    const srt::SrtlaNakPlan plan = srtlaPlanNak(floss, steady_clock::now(), p);

    ASSERT_EQ(plan.report.size(), 3u);
    EXPECT_EQ(plan.report[2], 2u);
    EXPECT_EQ(plan.confirmed, 3 + 3 + 1);
}

/// A never-reported loss is not crowded out of a full report by repeats of older
/// records, and the report still goes out in sequence order.
TEST(SrtlaPlanNak, FreshLossOutranksRepeats)
{
    const steady_clock::time_point now = steady_clock::now();

    srt::SrtlaNakParams p = makeParams();
    p.cap = 2; // room for exactly two single sequences

    std::deque<CRcvFreshLoss> floss;
    floss.push_back(makeAged(10, 10, 400)); // old, already reported, repeat is due
    floss.push_back(makeAged(20, 20, 400)); // ditto
    floss.push_back(makeAged(30, 30, 150)); // never reported
    floss[0].report_time = now - srt::sync::milliseconds_from(250);
    floss[1].report_time = now - srt::sync::milliseconds_from(250);

    const srt::SrtlaNakPlan plan = srtlaPlanNak(floss, now, p);

    ASSERT_EQ(plan.report.size(), 2u);
    EXPECT_NE(std::find(plan.report.begin(), plan.report.end(), size_t(2)), plan.report.end());
    EXPECT_EQ(plan.confirmed, 1);

    for (size_t i = 1; i < plan.report.size(); ++i)
        EXPECT_LT(plan.report[i - 1], plan.report[i]);
}

/// The prefix assumption behind the retire scan: a SPLIT keeps the deque ordered by
/// detection time, so retiring may never scan the whole container.
TEST(SrtlaPlanNak, SplitKeepsDequeOrderedByAge)
{
    std::deque<CRcvFreshLoss> floss;
    floss.push_back(makeAged(10, 10, 900));
    floss.push_back(makeAged(20, 30, 500));
    floss.push_back(makeAged(40, 40, 100));

    ASSERT_TRUE(CRcvFreshLoss::removeOne((floss), 25, NULL, NULL)); // splits record 1
    ASSERT_EQ(floss.size(), 4u);

    for (size_t i = 1; i < floss.size(); ++i)
        EXPECT_LE(floss[i - 1].timestamp, floss[i].timestamp) << "order broken at " << i;
}
