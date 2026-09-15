/**
 * The compact settlement encoding (docs/STRATEGY_action_proofs.md option
 * A): parse vectors, malformed inputs, and the pinned compact form of the
 * co-op parity fixture (the frontend's encoder must produce this exact
 * string, and parsing it must reproduce the fixture's consent hash).
 */

#include "moveprocessor.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace rog
{
namespace
{

std::vector<LoggedAction>
Parse (const std::string& text, const bool withActor = false)
{
  std::vector<LoggedAction> out;
  EXPECT_TRUE (ParseCompactActions (text, withActor, out)) << text;
  return out;
}

bool
Rejects (const std::string& text, const bool withActor = false)
{
  std::vector<LoggedAction> out;
  return !ParseCompactActions (text, withActor, out);
}

TEST (CompactActionsTests, NumpadMoves)
{
  const auto log = Parse ("m7;m8;m9;m4;m6;m1;m2;m3");
  ASSERT_EQ (log.size (), 8u);
  const int expected[8][2] = {
    {-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
  for (int k = 0; k < 8; k++)
    {
      EXPECT_EQ (log[k].action.type, Action::Type::Move);
      EXPECT_EQ (log[k].action.dx, expected[k][0]) << k;
      EXPECT_EQ (log[k].action.dy, expected[k][1]) << k;
      EXPECT_EQ (log[k].actor, 0);
    }
}

TEST (CompactActionsTests, AllCodes)
{
  const auto log = Parse ("p;w;g;uhealth_potion;e5,weapon;q12");
  ASSERT_EQ (log.size (), 6u);
  EXPECT_EQ (log[0].action.type, Action::Type::Pickup);
  EXPECT_EQ (log[1].action.type, Action::Type::Wait);
  EXPECT_EQ (log[2].action.type, Action::Type::EnterGate);
  EXPECT_EQ (log[3].action.type, Action::Type::UseItem);
  EXPECT_EQ (log[3].action.itemId, "health_potion");
  EXPECT_EQ (log[4].action.type, Action::Type::Equip);
  EXPECT_EQ (log[4].action.rowid, 5);
  EXPECT_EQ (log[4].action.slot, "weapon");
  EXPECT_EQ (log[5].action.type, Action::Type::Unequip);
  EXPECT_EQ (log[5].action.rowid, 12);
}

TEST (CompactActionsTests, RepeatsExpand)
{
  const auto log = Parse ("w*3;m6*2;p");
  ASSERT_EQ (log.size (), 6u);
  for (int k = 0; k < 3; k++)
    EXPECT_EQ (log[k].action.type, Action::Type::Wait);
  for (int k = 3; k < 5; k++)
    {
      EXPECT_EQ (log[k].action.type, Action::Type::Move);
      EXPECT_EQ (log[k].action.dx, 1);
    }
  EXPECT_EQ (log[5].action.type, Action::Type::Pickup);
}

TEST (CompactActionsTests, ActorPrefix)
{
  const auto log = Parse ("0:m6;1:w*2;0:g", true);
  ASSERT_EQ (log.size (), 4u);
  EXPECT_EQ (log[0].actor, 0);
  EXPECT_EQ (log[1].actor, 1);
  EXPECT_EQ (log[2].actor, 1);
  EXPECT_EQ (log[3].actor, 0);
  EXPECT_EQ (log[3].action.type, Action::Type::EnterGate);

  /* Required iff withActor.  */
  EXPECT_TRUE (Rejects ("m6;w", true));
  EXPECT_TRUE (Rejects ("0:m6;1:w", false));
}

TEST (CompactActionsTests, EmptyIsEmpty)
{
  EXPECT_TRUE (Parse ("").empty ());
  EXPECT_TRUE (Parse ("", true).empty ());
}

TEST (CompactActionsTests, MalformedRejected)
{
  for (const char* bad : {"x", "m5", "m", "m66", "w*0", "w*", "w*x", "*2",
                          "e5", "e,weapon", "e5,", "qabc", "q", "u", ";",
                          "w;", ";w", "w;;w", "p1", "g0", "-1:w", "a:w"})
    EXPECT_TRUE (Rejects (bad, std::string (bad).find (':') != std::string::npos))
        << bad;
  EXPECT_TRUE (Rejects ("w*100000"));
}

/* Pinned compact form of the co-op parity fixture (coop_parity_tests.cpp),
   in the canonical run-length form the frontend encoder emits.  */
const char* const COOP_FIXTURE_COMPACT =
  "0:e3,weapon;1:w;0:m6;1:m6;0:m6;1:m8;0:m6;1:m8;0:m9;1:m2;0:m8;1:m3;0:m8;"
  "1:m3;0:m8;1:m6;0:uhealth_potion;1:m6;0:m8;1:m6;0:m6;1:m6;0:m6;1:m6;0:m6;"
  "1:m6;0:m6;1:p;0:m6;1:m6;0:m6;1:m6;0:m6;1:m6;0:m6;1:m6;0:m6;1:m6;0:m6;"
  "1:m6;0:m6;1:m6;0:m6;1:m6;0:m6;1:m6;0:m6;1:m6;0:m3;1:m6;0:m9;1:m9;0:m9;"
  "1:m9;0:m8;1:m9;0:m8;1:m8;0:m6;1:m9;0:m9;1:m9;0:m9;1:m8;0:m8;1:m8;0:m9;"
  "1:m8;0:m7;1:m8;0:m1;1:m8;0:m1;1:m8;0:m1;1:m8;0:m1;1:m8;0:m1;1:m8;0:m4;"
  "1:m2;0:m1;1:m2;0:m1;1:m2;0:m1;1:m3;0:m2;1:m3;0:m2;1:m3;0:m2;1:m3;0:m2;"
  "1:m6;0:m2;1:m6;0:m2;1:m6;0:m2;1:m6;0:m2;1:m6;0:g;1:m6;1:uhealth_potion;"
  "1:m6;1:m4*13;1:m1*2;1:m2*8;1:g";

TEST (CompactActionsTests, CoopFixtureRoundTrip)
{
  const auto log = Parse (COOP_FIXTURE_COMPACT, true);
  ASSERT_EQ (log.size (), 132u);
  /* Same consent hash as the canonical form of the fixture.  */
  EXPECT_EQ (SettleLogHash (7, log),
             "3d92df40b001849551cc05dd5efc77905a9fe9dc59c92e46313e639425b6ab71");
}

} // anonymous namespace
} // namespace rog
