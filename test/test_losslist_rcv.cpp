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
