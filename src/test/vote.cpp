#include "test.h"
#include <gtest/gtest.h>

#include <base/system.h>
#include <game/server/vote.h>

TEST(Voting, MajorityPass)
{
	CVote Test("Test", "test", "test", true, 50, (int64)(time_get() + time_freq() * 2));
	EXPECT_FALSE(Test.Passed());
	Test.Vote(CVote::VOTE_PASS);
	EXPECT_TRUE(Test.Passed());
	EXPECT_FALSE(Test.Failed());
}