/**
 * Cross-language parity vectors for the multiplayer engine and the
 * settlement layer (SPEC_multiplayer_coop.md section 9).  The TypeScript
 * frontend (src/game/parity_test.ts, runCoopParityVector and friends)
 * replays the IDENTICAL pinned inputs; the canonical summary line printed
 * here must match the frontend's byte-for-byte, and the baked expectations
 * below guard against a silent drift on this side alone.
 */

#include "dungeongame.hpp"
#include "moveprocessor.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace rog
{
namespace
{

/**
 * Parses the canonical line encoding (spec section 7, one entry per
 * line, or ';'-separated) back into a merged log.  The fixture is pinned
 * in exactly the encoding the consent hash covers, so the same text also
 * exercises the hash.
 */
std::vector<LoggedAction>
ParseCanonicalLog (const std::string& text)
{
  std::vector<LoggedAction> log;
  std::string normalised = text;
  for (auto& c : normalised)
    if (c == ';')
      c = '\n';

  std::istringstream lines (normalised);
  std::string line;
  while (std::getline (lines, line))
    {
      if (line.empty ())
        continue;
      std::istringstream in (line);
      LoggedAction la;
      std::string type;
      in >> la.actor >> type;
      Action& a = la.action;
      if (type == "move")
        {
          a.type = Action::Type::Move;
          in >> a.dx >> a.dy;
        }
      else if (type == "pickup")
        a.type = Action::Type::Pickup;
      else if (type == "use")
        {
          a.type = Action::Type::UseItem;
          in >> a.itemId;
        }
      else if (type == "gate")
        a.type = Action::Type::EnterGate;
      else if (type == "wait")
        a.type = Action::Type::Wait;
      else if (type == "equip")
        {
          a.type = Action::Type::Equip;
          in >> a.rowid >> a.slot;
        }
      else if (type == "unequip")
        {
          a.type = Action::Type::Unequip;
          in >> a.rowid;
        }
      else
        ADD_FAILURE () << "bad canonical log line: " << line;
      log.push_back (la);
    }
  return log;
}

/* Pinned 2-player co-op run (generated once by a scripted greedy policy
   on the TS engine; both sides now replay this exact log).  Seed
   "coop-parity-4", depth 2, no entry gates (so participant 1 takes the
   ring spawn).  It covers every action type: an equip, a wait, potion
   use, raced pickups, kills by both participants, and both exits.  */
constexpr const char* COOP_FIXTURE_SEED = "coop-parity-4";
constexpr int COOP_FIXTURE_DEPTH = 2;
constexpr int64_t COOP_FIXTURE_VISIT_ID = 7;
const char* const COOP_FIXTURE_LOG =
  "0 equip 3 weapon;1 wait;0 move 1 0;1 move 1 0;0 move 1 0;1 move 0 -1;"
  "0 move 1 0;1 move 0 -1;0 move 1 -1;1 move 0 1;0 move 0 -1;1 move 1 1;"
  "0 move 0 -1;1 move 1 1;0 move 0 -1;1 move 1 0;0 use health_potion;"
  "1 move 1 0;0 move 0 -1;1 move 1 0;0 move 1 0;1 move 1 0;0 move 1 0;"
  "1 move 1 0;0 move 1 0;1 move 1 0;0 move 1 0;1 pickup;0 move 1 0;"
  "1 move 1 0;0 move 1 0;1 move 1 0;0 move 1 0;1 move 1 0;0 move 1 0;"
  "1 move 1 0;0 move 1 0;1 move 1 0;0 move 1 0;1 move 1 0;0 move 1 0;"
  "1 move 1 0;0 move 1 0;1 move 1 0;0 move 1 0;1 move 1 0;0 move 1 0;"
  "1 move 1 0;0 move 1 1;1 move 1 0;0 move 1 -1;1 move 1 -1;0 move 1 -1;"
  "1 move 1 -1;0 move 0 -1;1 move 1 -1;0 move 0 -1;1 move 0 -1;0 move 1 0;"
  "1 move 1 -1;0 move 1 -1;1 move 1 -1;0 move 1 -1;1 move 0 -1;0 move 0 -1;"
  "1 move 0 -1;0 move 1 -1;1 move 0 -1;0 move -1 -1;1 move 0 -1;"
  "0 move -1 1;1 move 0 -1;0 move -1 1;1 move 0 -1;0 move -1 1;1 move 0 -1;"
  "0 move -1 1;1 move 0 -1;0 move -1 1;1 move 0 -1;0 move -1 0;1 move 0 1;"
  "0 move -1 1;1 move 0 1;0 move -1 1;1 move 0 1;0 move -1 1;1 move 1 1;"
  "0 move 0 1;1 move 1 1;0 move 0 1;1 move 1 1;0 move 0 1;1 move 1 1;"
  "0 move 0 1;1 move 1 0;0 move 0 1;1 move 1 0;0 move 0 1;1 move 1 0;"
  "0 move 0 1;1 move 1 0;0 move 0 1;1 move 1 0;0 gate;1 move 1 0;"
  "1 use health_potion;1 move 1 0;1 move -1 0;1 move -1 0;1 move -1 0;"
  "1 move -1 0;1 move -1 0;1 move -1 0;1 move -1 0;1 move -1 0;1 move -1 0;"
  "1 move -1 0;1 move -1 0;1 move -1 0;1 move -1 0;1 move -1 1;1 move -1 1;"
  "1 move 0 1;1 move 0 1;1 move 0 1;1 move 0 1;1 move 0 1;1 move 0 1;"
  "1 move 0 1;1 move 0 1;1 gate;";

std::vector<DungeonGame::PlayerSetup>
CoopFixtureSetups ()
{
  DungeonGame::PlayerSetup alice;
  alice.stats.level = 2;
  alice.stats.strength = 10;
  alice.stats.dexterity = 11;
  alice.stats.constitution = 10;
  alice.stats.intelligence = 10;
  alice.stats.equipAttack = 5;
  alice.stats.equipDefense = 2;
  alice.hp = 90;
  alice.maxHp = 100;
  alice.potions = {{"health_potion", 2}};
  alice.inventory = {
    {1, "short_sword", "weapon"},
    {2, "leather_armor", "body"},
    {3, "iron_sword", "bag"},
  };

  DungeonGame::PlayerSetup bob;
  bob.stats.level = 1;
  bob.stats.strength = 12;
  bob.stats.dexterity = 9;
  bob.stats.constitution = 11;
  bob.stats.intelligence = 9;
  bob.stats.equipAttack = 5;
  bob.stats.equipDefense = 2;
  bob.hp = 105;
  bob.maxHp = 105;
  bob.potions = {{"health_potion", 3}};
  bob.inventory = {
    {11, "short_sword", "weapon"},
    {12, "leather_armor", "body"},
  };

  return {alice, bob};
}

TEST (CoopParityTests, CoopRunVector)
{
  const auto log = ParseCanonicalLog (COOP_FIXTURE_LOG);
  ASSERT_EQ (log.size (), 132u);

  auto game = DungeonGame::ReplayMulti (COOP_FIXTURE_SEED, COOP_FIXTURE_DEPTH,
                                         CoopFixtureSetups (), log);

  /* The whole log must replay (a prefix stop means the engines disagree
     on validity somewhere).  */
  ASSERT_EQ (game.GetMergedLog ().size (), log.size ());

  std::vector<int64_t> damages;
  for (int i = 0; i < game.GetPlayerCount (); i++)
    damages.push_back (game.GetDamageDealt (i));
  const auto xpShares = SplitPool (game.GetXpPool (), damages);
  const auto goldShares = SplitPool (game.GetKillGoldPool (), damages);

  /* Canonical summary line, diffed byte-for-byte against the frontend.  */
  std::string line = "PARITY-COOP";
  for (int i = 0; i < game.GetPlayerCount (); i++)
    {
      char buf[256];
      std::snprintf (buf, sizeof (buf),
                     " p%d[survived=%d xp=%lld gold=%lld kills=%d hp=%d"
                     " maxHp=%d dmg=%d exit=%s]",
                     i, game.HasPlayerExited (i) ? 1 : 0,
                     static_cast<long long> (xpShares[i]),
                     static_cast<long long> (game.GetTotalGold (i)
                                             + goldShares[i]),
                     game.GetTotalKills (i), game.GetPlayerHp (i),
                     game.GetPlayerMaxHp (i), game.GetDamageDealt (i),
                     game.GetExitGate (i).c_str ());
      line += buf;
    }
  line += " pools[xp=" + std::to_string (game.GetXpPool ())
        + " gold=" + std::to_string (game.GetKillGoldPool ()) + "]";
  line += " turns=" + std::to_string (game.GetTurnCount ());
  line += " hash=" + SettleLogHash (COOP_FIXTURE_VISIT_ID, log);
  std::printf ("%s\n", line.c_str ());

  /* Baked expectations (confirmed identical on the TS side).  */
  EXPECT_EQ (line,
             "PARITY-COOP"
             " p0[survived=1 xp=44 gold=2 kills=3 hp=95 maxHp=100 dmg=106"
             " exit=south]"
             " p1[survived=1 xp=58 gold=6 kills=3 hp=102 maxHp=105 dmg=137"
             " exit=south]"
             " pools[xp=102 gold=4] turns=132"
             " hash=3d92df40b001849551cc05dd5efc77905a9fe9dc59c92e46313e639425b6ab71");
}

TEST (CoopParityTests, SettleLogHashVector)
{
  /* One entry of every action type, so the whole canonical encoding is
     locked (pinned in parity_test.ts as well).  */
  const auto log = ParseCanonicalLog (
      "0 move 1 0\n1 move -1 -1\n0 pickup\n1 use health_potion\n"
      "0 equip 5 weapon\n1 unequip 12\n0 wait\n1 gate\n0 gate\n");
  ASSERT_EQ (log.size (), 9u);

  std::string data = "rog-settle-v1\n42\n";
  for (const auto& la : log)
    data += CanonicalActionLine (la.actor, la.action);
  EXPECT_EQ (data,
             "rog-settle-v1\n42\n"
             "0 move 1 0\n1 move -1 -1\n0 pickup\n1 use health_potion\n"
             "0 equip 5 weapon\n1 unequip 12\n0 wait\n1 gate\n0 gate\n");

  EXPECT_EQ (SettleLogHash (42, log), "7ce422e908ecfe7bd587de2740aa91f94a8631ead9bd80f794166b0ad5267297");
  EXPECT_EQ (SettleLogHash (42, {}),
             "892bdea5bc97abd2bc0d0b4991d445aaf42c6d5ee7e68fa572ec5e1ea3d81ffd");
}

} // anonymous namespace
} // namespace rog
