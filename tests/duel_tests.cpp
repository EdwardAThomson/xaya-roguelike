/**
 * Behavioural tests for duels (SPEC_multiplayer_pvp.md): the round
 * protocol the engine enforces, the stake escrow on the move layer, and
 * the settlement that pays the pot out.  The cross-language vectors live
 * in duel_parity_tests.cpp; these are the rules that do not need to be
 * byte-identical with the frontend, only correct.
 */

#include "moveprocessor.hpp"
#include "testutils.hpp"

#include "dungeongame.hpp"
#include "hash.hpp"
#include "items.hpp"

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <json/json.h>

#include <string>
#include <vector>

namespace rog
{
namespace
{

Action
MoveAction (const int dx, const int dy)
{
  Action a;
  a.type = Action::Type::Move;
  a.dx = dx;
  a.dy = dy;
  return a;
}

Action
WaitAction ()
{
  Action a;
  a.type = Action::Type::Wait;
  return a;
}

Action
CommitAction (const std::string& hex)
{
  Action a;
  a.type = Action::Type::Commit;
  a.hex = hex;
  return a;
}

Action
RevealAction (const std::string& hex)
{
  Action a;
  a.type = Action::Type::Reveal;
  a.hex = hex;
  return a;
}

std::vector<DungeonGame::PlayerSetup>
TwoSetups (const std::string& entryDir = "south")
{
  DungeonGame::PlayerSetup a;
  a.stats.level = 3;
  a.stats.strength = 13;
  a.stats.dexterity = 11;
  a.stats.constitution = 10;
  a.stats.intelligence = 10;
  a.stats.equipAttack = 5;
  a.stats.equipDefense = 2;
  a.hp = 100;
  a.maxHp = 100;
  a.potions = {{"health_potion", 1}};
  a.entryDir = entryDir;

  DungeonGame::PlayerSetup b = a;
  b.stats.level = 2;
  b.stats.strength = 12;
  b.stats.dexterity = 9;
  b.hp = 95;
  b.maxHp = 105;
  b.entryDir = entryDir;

  return {a, b};
}

constexpr int64_t VISIT = 11;
constexpr const char* SEED = "duel-parity-1";
constexpr int DEPTH = 3;

/** Salt k, as a valid 16-byte hex string.  */
std::string
Salt (const int k)
{
  return Sha256Hex ("duel-test-" + std::to_string (k)).substr (0, 32);
}

/** Drives one full round in which both participants do `a0` and `a1`.  */
void
PlayRound (DungeonGame& game, const Action& a0, const Action& a1,
           const int saltBase)
{
  const int t = game.GetRoundIndex ();
  const Action acts[2] = {a0, a1};
  for (int i = 0; i < 2; i++)
    ASSERT_TRUE (game.ProcessAction (i, CommitAction (
        DuelCommitHash (VISIT, t, i, acts[i], Salt (saltBase + i)))));
  for (int i = 0; i < 2; i++)
    ASSERT_TRUE (game.ProcessAction (i, RevealAction (Salt (saltBase + i))));
  for (int i = 0; i < 2; i++)
    {
      if (game.IsGameOver () || !game.IsPlayerActive (i))
        continue;
      ASSERT_TRUE (game.ProcessAction (i, acts[i]));
    }
}

/* ************************************************************************ */

class DuelEngineTests : public testing::Test
{
};

TEST_F (DuelEngineTests, CoopNeverAcceptsCommitOrReveal)
{
  auto game = DungeonGame::CreateMulti (SEED, DEPTH, TwoSetups ());
  EXPECT_FALSE (game.IsDuel ());
  EXPECT_EQ (game.GetPhase (), DungeonGame::Phase::Act);

  EXPECT_FALSE (game.ProcessAction (0, CommitAction (std::string (64, 'a'))));
  EXPECT_FALSE (game.ProcessAction (0, RevealAction (std::string (32, 'b'))));

  /* A plain co-op action still works, and nothing was logged by the two
     rejected protocol entries.  */
  EXPECT_TRUE (game.ProcessAction (0, WaitAction ()));
  EXPECT_EQ (game.GetMergedLog ().size (), 1u);
}

TEST_F (DuelEngineTests, RoundStepsAreOrdered)
{
  auto game = DungeonGame::CreateDuel (SEED, DEPTH, TwoSetups (), VISIT);
  ASSERT_TRUE (game.IsDuel ());
  ASSERT_EQ (game.GetPhase (), DungeonGame::Phase::Commit);

  const Action act = WaitAction ();
  const std::string h0 = DuelCommitHash (VISIT, 0, 0, act, Salt (0));

  /* An action, or a reveal, before the commits are in: refused.  */
  EXPECT_FALSE (game.ProcessAction (0, act));
  EXPECT_FALSE (game.ProcessAction (0, RevealAction (Salt (0))));

  /* Out of turn: participant 1 may not commit before participant 0.  */
  EXPECT_FALSE (game.ProcessAction (1, CommitAction (h0)));

  ASSERT_TRUE (game.ProcessAction (0, CommitAction (h0)));
  EXPECT_EQ (game.GetPhase (), DungeonGame::Phase::Commit);
  ASSERT_TRUE (game.ProcessAction (1, CommitAction (
      DuelCommitHash (VISIT, 0, 1, act, Salt (1)))));
  EXPECT_EQ (game.GetPhase (), DungeonGame::Phase::Reveal);

  /* Now a commit is what is out of place.  */
  EXPECT_FALSE (game.ProcessAction (0, CommitAction (h0)));
  ASSERT_TRUE (game.ProcessAction (0, RevealAction (Salt (0))));
  ASSERT_TRUE (game.ProcessAction (1, RevealAction (Salt (1))));
  EXPECT_EQ (game.GetPhase (), DungeonGame::Phase::Act);

  ASSERT_TRUE (game.ProcessAction (0, act));
  ASSERT_TRUE (game.ProcessAction (1, act));

  /* Round closed: back to committing, one round further on.  */
  EXPECT_EQ (game.GetPhase (), DungeonGame::Phase::Commit);
  EXPECT_EQ (game.GetRoundIndex (), 1);
}

TEST_F (DuelEngineTests, MalformedCommitAndRevealRejected)
{
  auto game = DungeonGame::CreateDuel (SEED, DEPTH, TwoSetups (), VISIT);

  EXPECT_FALSE (game.ProcessAction (0, CommitAction ("")));
  EXPECT_FALSE (game.ProcessAction (0, CommitAction (std::string (63, 'a'))));
  EXPECT_FALSE (game.ProcessAction (0, CommitAction (std::string (64, 'A'))));
  EXPECT_FALSE (game.ProcessAction (0, CommitAction (std::string (64, 'z'))));

  const Action act = WaitAction ();
  ASSERT_TRUE (game.ProcessAction (0, CommitAction (
      DuelCommitHash (VISIT, 0, 0, act, Salt (0)))));
  ASSERT_TRUE (game.ProcessAction (1, CommitAction (
      DuelCommitHash (VISIT, 0, 1, act, Salt (1)))));

  /* A salt has to be 16 bytes of lowercase hex.  */
  EXPECT_FALSE (game.ProcessAction (0, RevealAction (std::string (31, 'b'))));
  EXPECT_FALSE (game.ProcessAction (0, RevealAction (std::string (32, 'X'))));
  EXPECT_TRUE (game.ProcessAction (0, RevealAction (Salt (0))));
}

TEST_F (DuelEngineTests, RevealMustOpenTheCommitment)
{
  auto game = DungeonGame::CreateDuel (SEED, DEPTH, TwoSetups (), VISIT);
  const Action act = WaitAction ();

  ASSERT_TRUE (game.ProcessAction (0, CommitAction (
      DuelCommitHash (VISIT, 0, 0, act, Salt (0)))));
  ASSERT_TRUE (game.ProcessAction (1, CommitAction (
      DuelCommitHash (VISIT, 0, 1, act, Salt (1)))));

  /* Participant 0 reveals a DIFFERENT salt from the one it committed
     with.  The reveal itself is well-formed, so it is accepted; the
     action that follows is what fails to open the commitment.  */
  ASSERT_TRUE (game.ProcessAction (0, RevealAction (Salt (99))));
  ASSERT_TRUE (game.ProcessAction (1, RevealAction (Salt (1))));
  EXPECT_FALSE (game.ProcessAction (0, act));
}

TEST_F (DuelEngineTests, ActionMustMatchTheCommitment)
{
  auto game = DungeonGame::CreateDuel (SEED, DEPTH, TwoSetups (), VISIT);
  const Action committed = WaitAction ();

  ASSERT_TRUE (game.ProcessAction (0, CommitAction (
      DuelCommitHash (VISIT, 0, 0, committed, Salt (0)))));
  ASSERT_TRUE (game.ProcessAction (1, CommitAction (
      DuelCommitHash (VISIT, 0, 1, committed, Salt (1)))));
  ASSERT_TRUE (game.ProcessAction (0, RevealAction (Salt (0))));
  ASSERT_TRUE (game.ProcessAction (1, RevealAction (Salt (1))));

  /* Committed to a wait, then tried to move: the replay fails.  This is
     the whole point of the commitment -- the choice is sealed before the
     round's entropy is known.  */
  EXPECT_FALSE (game.ProcessAction (0, MoveAction (0, -1)));
  EXPECT_TRUE (game.ProcessAction (0, committed));
}

TEST_F (DuelEngineTests, CommitmentIsBoundToVisitAndRound)
{
  const Action act = WaitAction ();
  auto game = DungeonGame::CreateDuel (SEED, DEPTH, TwoSetups (), VISIT);

  /* A commitment made for a different visit, or a different round, does
     not open here, so it can never be replayed across either.  */
  ASSERT_TRUE (game.ProcessAction (0, CommitAction (
      DuelCommitHash (VISIT + 1, 0, 0, act, Salt (0)))));
  ASSERT_TRUE (game.ProcessAction (1, CommitAction (
      DuelCommitHash (VISIT, 1, 1, act, Salt (1)))));
  ASSERT_TRUE (game.ProcessAction (0, RevealAction (Salt (0))));
  ASSERT_TRUE (game.ProcessAction (1, RevealAction (Salt (1))));
  EXPECT_FALSE (game.ProcessAction (0, act));

  EXPECT_NE (DuelCommitHash (VISIT, 0, 0, act, Salt (0)),
             DuelCommitHash (VISIT, 0, 1, act, Salt (0)));
}

TEST_F (DuelEngineTests, OvertakenActionBecomesAWait)
{
  auto game = DungeonGame::CreateDuel (SEED, DEPTH, TwoSetups (), VISIT);

  /* Participant 0 spawns at (56, 38) with participant 1 directly above;
     stepping DOWN lands on the gate tile, stepping further down leaves
     the grid.  Committing to that impossible move is applied as a wait
     while the log keeps the action the commitment covers.  */
  const int x0 = game.GetPlayerX (0), y0 = game.GetPlayerY (0);
  const Action offGrid = MoveAction (0, 1);
  const Action wait = WaitAction ();

  /* Walk participant 0 onto the bottom row first.  */
  PlayRound (game, MoveAction (0, 1), wait, 0);
  ASSERT_EQ (game.GetPlayerY (0), y0 + 1);
  ASSERT_EQ (game.GetPlayerX (0), x0);
  ASSERT_EQ (game.GetPlayerY (0), Dungeon::HEIGHT - 1);

  const int before = game.GetMergedLog ().size ();
  PlayRound (game, offGrid, wait, 10);

  /* It did not move, and the entry in the log is the committed move, not
     a substituted wait -- otherwise the commitment would stop opening.  */
  EXPECT_EQ (game.GetPlayerY (0), Dungeon::HEIGHT - 1);
  ASSERT_EQ (game.GetMergedLog ().size (), before + 6u);
  const auto& entry = game.GetMergedLog ()[before + 4];
  EXPECT_EQ (entry.actor, 0);
  EXPECT_EQ (entry.action.type, Action::Type::Move);
  EXPECT_EQ (entry.action.dy, 1);

  /* And the round still closed normally.  */
  EXPECT_EQ (game.GetPhase (), DungeonGame::Phase::Commit);
  EXPECT_EQ (game.GetRoundIndex (), 2);
}

TEST_F (DuelEngineTests, BumpingAHostileParticipantAttacks)
{
  auto game = DungeonGame::CreateDuel (SEED, DEPTH, TwoSetups (), VISIT);
  const int hpBefore = game.GetPlayerHp (1);

  /* Participant 1 is directly above participant 0.  */
  ASSERT_EQ (game.GetPlayerX (0), game.GetPlayerX (1));
  ASSERT_EQ (game.GetPlayerY (0), game.GetPlayerY (1) + 1);

  PlayRound (game, MoveAction (0, -1), WaitAction (), 0);

  /* Attacking does not move the attacker, and the damage is tracked apart
     from the monster-damage pools.  */
  EXPECT_EQ (game.GetPlayerY (0), game.GetPlayerY (1) + 1);
  EXPECT_LT (game.GetPlayerHp (1), hpBefore);
  EXPECT_GT (game.GetPvpDamage (0), 0);
  EXPECT_EQ (game.GetDamageDealt (0), 0);
  EXPECT_EQ (game.GetXpPool (), 0);
}

TEST_F (DuelEngineTests, CoopBlocksInsteadOfAttacking)
{
  auto game = DungeonGame::CreateMulti (SEED, DEPTH, TwoSetups ());
  const int hpBefore = game.GetPlayerHp (1);

  /* The same bump in a co-op run is simply an invalid move, exactly as
     it has always been: this is what every settled co-op run verified
     against, so it cannot change.  */
  EXPECT_FALSE (game.ProcessAction (0, MoveAction (0, -1)));
  EXPECT_EQ (game.GetPlayerHp (1), hpBefore);
}

TEST_F (DuelEngineTests, SaltsDecideTheRoundsEntropy)
{
  /* Same seed, same actions, different salts: different rolls.  Neither
     player can predict the round when choosing, because the other salt is
     committed but unrevealed.  */
  auto a = DungeonGame::CreateDuel (SEED, DEPTH, TwoSetups (), VISIT);
  auto b = DungeonGame::CreateDuel (SEED, DEPTH, TwoSetups (), VISIT);

  for (int r = 0; r < 6; r++)
    {
      PlayRound (a, MoveAction (0, -1), MoveAction (0, 1), r * 2);
      PlayRound (b, MoveAction (0, -1), MoveAction (0, 1), 500 + r * 2);
    }

  EXPECT_NE (a.GetPvpDamage (0), b.GetPvpDamage (0));
}

TEST_F (DuelEngineTests, AbsentDuellistLosesImmediately)
{
  auto game = DungeonGame::CreateDuel (SEED, DEPTH, TwoSetups (), VISIT);
  PlayRound (game, WaitAction (), WaitAction (), 0);
  ASSERT_EQ (game.GetDuelWinner (), -1);

  game.MarkAbsent (1);
  EXPECT_TRUE (game.IsGameOver ());
  EXPECT_EQ (game.GetDuelWinner (), 0);
}

/* ************************************************************************ */

/**
 * The move layer: hosting a duel escrows the host's stake, joining matches
 * it, and every path that ends a duel without a winner gives the gold
 * back (SPEC_multiplayer_pvp.md section 5).
 */
class DuelMoveTests : public DBTest
{

protected:

  int64_t nextVisitId = 1;

  void ProcessMove (const std::string& name, const std::string& moveJson,
                    unsigned height = 100,
                    const std::string& txid = "deadbeef")
  {
    Json::Value obj (Json::objectValue);
    obj["name"] = name;
    obj["txid"] = txid;
    obj["move"] = ParseJson (moveJson);

    Json::Value moves (Json::arrayValue);
    moves.append (obj);

    MoveProcessor proc (GetHandle (), height, nextVisitId);
    proc.ProcessAll (moves);
  }

  /** Runs only the timeout pass for a block at `height`.  */
  void RunTimeouts (const unsigned height)
  {
    MoveProcessor proc (GetHandle (), height, nextVisitId);
    proc.ProcessAll (Json::Value (Json::arrayValue));
  }

  void SetUp () override
  {
    ProcessMove ("alice", R"({"r": {}})");
    ProcessMove ("bob", R"({"r": {}})");
    ProcessMove ("alice", R"({"d": {"depth": 3, "dir": "east"}})",
                 200, "seed123");
    Execute ("UPDATE `segments` SET `confirmed` = 1, `max_players` = 2"
             " WHERE `world_x` = 1 AND `world_y` = 0");
    Execute ("UPDATE `players` SET `gold` = 100");
  }

  int64_t Gold (const std::string& name)
  {
    return QueryInt (
      "SELECT `gold` FROM `players` WHERE `name` = '" + name + "'");
  }

};

TEST_F (DuelMoveTests, HostingADuelEscrowsTheStake)
{
  ProcessMove ("alice", R"({"v": {"dir": "east", "mode": "duel",
                                  "stake": 30}})", 300);

  EXPECT_EQ (QueryString ("SELECT `mode` FROM `visits` WHERE `id` = 1"),
             "duel");
  EXPECT_EQ (QueryInt ("SELECT `stake` FROM `visits` WHERE `id` = 1"), 30);
  EXPECT_EQ (QueryInt ("SELECT `pot` FROM `visits` WHERE `id` = 1"), 30);
  EXPECT_EQ (Gold ("alice"), 70);
}

TEST_F (DuelMoveTests, CoopVisitIsUnchanged)
{
  ProcessMove ("alice", R"({"v": {"dir": "east"}})", 300);

  EXPECT_EQ (QueryString ("SELECT `mode` FROM `visits` WHERE `id` = 1"),
             "coop");
  EXPECT_EQ (QueryInt ("SELECT `stake` FROM `visits` WHERE `id` = 1"), 0);
  EXPECT_EQ (QueryInt ("SELECT `pot` FROM `visits` WHERE `id` = 1"), 0);
  EXPECT_EQ (Gold ("alice"), 100);
}

TEST_F (DuelMoveTests, StakeOutsideADuelRejected)
{
  ProcessMove ("alice", R"({"v": {"dir": "east", "stake": 30}})", 300);
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `visits`"), 0);
  EXPECT_EQ (Gold ("alice"), 100);
}

TEST_F (DuelMoveTests, UnknownModeRejected)
{
  ProcessMove ("alice", R"({"v": {"dir": "east", "mode": "brawl"}})", 300);
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `visits`"), 0);
}

TEST_F (DuelMoveTests, NegativeStakeRejected)
{
  ProcessMove ("alice", R"({"v": {"dir": "east", "mode": "duel",
                                  "stake": -5}})", 300);
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `visits`"), 0);
}

TEST_F (DuelMoveTests, UnaffordableStakeRejected)
{
  ProcessMove ("alice", R"({"v": {"dir": "east", "mode": "duel",
                                  "stake": 500}})", 300);
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `visits`"), 0);
  EXPECT_EQ (Gold ("alice"), 100);
}

TEST_F (DuelMoveTests, DuelNeedsATwoPlayerArena)
{
  Execute ("UPDATE `segments` SET `max_players` = 3"
           " WHERE `world_x` = 1 AND `world_y` = 0");
  ProcessMove ("alice", R"({"v": {"dir": "east", "mode": "duel",
                                  "stake": 10}})", 300);
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `visits`"), 0);
  EXPECT_EQ (Gold ("alice"), 100);
}

TEST_F (DuelMoveTests, JoiningMatchesTheStake)
{
  ProcessMove ("alice", R"({"v": {"dir": "east", "mode": "duel",
                                  "stake": 30}})", 300);
  ProcessMove ("bob", R"({"j": {"id": 1, "dir": "east"}})", 301);

  EXPECT_EQ (QueryInt ("SELECT `pot` FROM `visits` WHERE `id` = 1"), 60);
  EXPECT_EQ (Gold ("alice"), 70);
  EXPECT_EQ (Gold ("bob"), 70);
  EXPECT_EQ (QueryString ("SELECT `status` FROM `visits` WHERE `id` = 1"),
             "active");
}

TEST_F (DuelMoveTests, JoinerWhoCannotCoverTheStakeIsRejected)
{
  ProcessMove ("alice", R"({"v": {"dir": "east", "mode": "duel",
                                  "stake": 90}})", 300);
  Execute ("UPDATE `players` SET `gold` = 10 WHERE `name` = 'bob'");
  ProcessMove ("bob", R"({"j": {"id": 1, "dir": "east"}})", 301);

  EXPECT_EQ (QueryInt (
    "SELECT COUNT(*) FROM `visit_participants` WHERE `visit_id` = 1"), 1);
  EXPECT_EQ (QueryInt ("SELECT `pot` FROM `visits` WHERE `id` = 1"), 90);
  EXPECT_EQ (Gold ("bob"), 10);
}

TEST_F (DuelMoveTests, CancellingAnOpenDuelRefundsTheHost)
{
  ProcessMove ("alice", R"({"v": {"dir": "east", "mode": "duel",
                                  "stake": 30}})", 300);
  ASSERT_EQ (Gold ("alice"), 70);

  ProcessMove ("alice", R"({"lv": {"id": 1}})", 310);

  EXPECT_EQ (QueryString ("SELECT `status` FROM `visits` WHERE `id` = 1"),
             "cancelled");
  EXPECT_EQ (QueryInt ("SELECT `pot` FROM `visits` WHERE `id` = 1"), 0);
  EXPECT_EQ (Gold ("alice"), 100);
}

TEST_F (DuelMoveTests, ExpiringAnOpenDuelRefundsTheHost)
{
  ProcessMove ("alice", R"({"v": {"dir": "east", "mode": "duel",
                                  "stake": 30}})", 300);
  ASSERT_EQ (Gold ("alice"), 70);

  RunTimeouts (300 + MoveProcessor::VISIT_OPEN_TIMEOUT);

  EXPECT_EQ (QueryString ("SELECT `status` FROM `visits` WHERE `id` = 1"),
             "expired");
  EXPECT_EQ (QueryInt ("SELECT `pot` FROM `visits` WHERE `id` = 1"), 0);
  EXPECT_EQ (Gold ("alice"), 100);
}

TEST_F (DuelMoveTests, UnsettleableDuelIsVoidedAndRefunded)
{
  ProcessMove ("alice", R"({"v": {"dir": "east", "mode": "duel",
                                  "stake": 30}})", 300);
  ProcessMove ("bob", R"({"j": {"id": 1, "dir": "east"}})", 301);
  ASSERT_EQ (Gold ("alice"), 70);
  ASSERT_EQ (Gold ("bob"), 70);

  /* Neither side ever settles: both vanished, so the pot would be locked
     forever.  It is returned and the duel is void (spec section 12.5).  */
  RunTimeouts (301 + MoveProcessor::DUEL_ABANDON_TIMEOUT);

  EXPECT_EQ (QueryString ("SELECT `status` FROM `visits` WHERE `id` = 1"),
             "voided");
  EXPECT_EQ (QueryInt ("SELECT `pot` FROM `visits` WHERE `id` = 1"), 0);
  EXPECT_EQ (Gold ("alice"), 100);
  EXPECT_EQ (Gold ("bob"), 100);
  EXPECT_EQ (QueryInt (
    "SELECT `in_channel` FROM `players` WHERE `name` = 'alice'"), 0);

  /* Nobody was moved and nobody was penalised: a void is not a death.  */
  EXPECT_EQ (QueryInt (
    "SELECT `deaths` FROM `players` WHERE `name` = 'alice'"), 0);
  EXPECT_EQ (QueryInt (
    "SELECT `deaths` FROM `players` WHERE `name` = 'bob'"), 0);
}

TEST_F (DuelMoveTests, CheckpointingDuelIsNotVoided)
{
  /* The void counts SILENCE, not age (spec decision 5).  A duel that runs
     long but keeps checkpointing is alive, and must not be voided out from
     under the players -- the pot is theirs to fight for.  */
  ProcessMove ("alice", R"({"v": {"dir": "east", "mode": "duel",
                                  "stake": 30}})", 300);
  ProcessMove ("bob", R"({"j": {"id": 1, "dir": "east"}})", 301);

  /* A checkpoint well after the duel started, but before the window.  */
  const unsigned late = 301 + MoveProcessor::DUEL_ABANDON_TIMEOUT - 10;
  ProcessMove ("bob",
               R"({"sc": {"id": 1, "h": ")" + std::string (64, 'a')
               + R"(", "n": 0}})", late);

  /* Past the point where AGE alone would have voided it.  */
  RunTimeouts (301 + MoveProcessor::DUEL_ABANDON_TIMEOUT);
  EXPECT_EQ (QueryString ("SELECT `status` FROM `visits` WHERE `id` = 1"),
             "active");
  EXPECT_EQ (QueryInt ("SELECT `pot` FROM `visits` WHERE `id` = 1"), 60);

  /* Once the checkpoint itself goes that stale, the duel is gone.  */
  RunTimeouts (late + MoveProcessor::DUEL_ABANDON_TIMEOUT);
  EXPECT_EQ (QueryString ("SELECT `status` FROM `visits` WHERE `id` = 1"),
             "voided");
  EXPECT_EQ (Gold ("alice"), 100);
  EXPECT_EQ (Gold ("bob"), 100);
}

TEST_F (DuelMoveTests, CoopVisitDoesNotTimeOutOnAConfirmedSegment)
{
  /* The duel void must not have introduced a timeout for co-op runs,
     which deliberately never expire on a confirmed segment.  */
  ProcessMove ("alice", R"({"v": {"dir": "east"}})", 300);
  ProcessMove ("bob", R"({"j": {"id": 1, "dir": "east"}})", 301);

  RunTimeouts (301 + MoveProcessor::DUEL_ABANDON_TIMEOUT);

  EXPECT_EQ (QueryString ("SELECT `status` FROM `visits` WHERE `id` = 1"),
             "active");
}

/* ************************************************************************ */

/**
 * Settlement: the winner takes the pot and the duel XP, the loser takes
 * the ordinary death outcome, and the GSP recomputes the winner rather
 * than believing either claim (spec sections 5, 5a and 7).
 */
class DuelSettleTests : public DuelMoveTests
{

protected:

  /** The log the duel produced, and the game it produced it on.  */
  std::vector<LoggedAction> log;

  void SetUp () override
  {
    DuelMoveTests::SetUp ();
    ProcessMove ("alice", R"({"v": {"dir": "east", "mode": "duel",
                                    "stake": 30}})", 300);
    ProcessMove ("bob", R"({"j": {"id": 1, "dir": "east"}})", 301);
  }

  /** Rebuilds the duel exactly as the settlement replay will.  */
  DungeonGame BuildDuel ()
  {
    sqlite3* dbh = GetHandle ();
    const std::string seed = QueryString (
      "SELECT `seed` FROM `segments` WHERE `world_x` = 1 AND `world_y` = 0");
    const int depth = QueryInt (
      "SELECT `depth` FROM `segments` WHERE `world_x` = 1 AND `world_y` = 0");

    std::vector<Gate> constraints;
    const std::string cdir = QueryString (
      "SELECT COALESCE(`constraint_dir`, '') FROM `segments`"
      " WHERE `world_x` = 1 AND `world_y` = 0");
    if (!cdir.empty ())
      {
        Gate g;
        g.direction = cdir;
        g.x = QueryInt (
          "SELECT `x` FROM `segment_gates` WHERE `segment_x` = 1"
          " AND `segment_y` = 0 AND `direction` = '" + cdir + "'");
        g.y = QueryInt (
          "SELECT `y` FROM `segment_gates` WHERE `segment_x` = 1"
          " AND `segment_y` = 0 AND `direction` = '" + cdir + "'");
        constraints.push_back (g);
      }

    std::vector<DungeonGame::PlayerSetup> setups;
    for (const std::string p : {"alice", "bob"})
      {
        DungeonGame::PlayerSetup s;
        s.entryDir = QueryString (
          "SELECT `entry_direction` FROM `visit_participants`"
          " WHERE `visit_id` = 1 AND `name` = '" + p + "'");
        s.stats = ComputePlayerStats (dbh, p);
        s.hp = QueryInt (
          "SELECT `hp` FROM `players` WHERE `name` = '" + p + "'");
        s.maxHp = QueryInt (
          "SELECT `max_hp` FROM `players` WHERE `name` = '" + p + "'");
        for (const auto& [pid, pqty] : GetPlayerPotions (dbh, p))
          s.potions.push_back ({pid, pqty});

        sqlite3_stmt* stmt;
        sqlite3_prepare_v2 (dbh,
          "SELECT `rowid`, `item_id`, `slot` FROM `inventory`"
          " WHERE `name` = ?1 ORDER BY `rowid`",
          -1, &stmt, nullptr);
        sqlite3_bind_text (stmt, 1, p.c_str (), -1, SQLITE_TRANSIENT);
        while (sqlite3_step (stmt) == SQLITE_ROW)
          {
            EntryInventoryItem item;
            item.rowid = sqlite3_column_int64 (stmt, 0);
            item.itemId = reinterpret_cast<const char*> (
                sqlite3_column_text (stmt, 1));
            item.slot = reinterpret_cast<const char*> (
                sqlite3_column_text (stmt, 2));
            s.inventory.push_back (item);
          }
        sqlite3_finalize (stmt);

        setups.push_back (s);
      }
    return DungeonGame::CreateDuel (seed, depth, setups, 1, constraints);
  }

  /**
   * Fights the duel out: both bump into each other every round until one
   * dies.  Fills `log` with the merged log, commit and reveal entries
   * included, and returns the finished game.
   */
  DungeonGame FightToTheDeath ()
  {
    auto game = BuildDuel ();
    log.clear ();

    for (int r = 0; r < 200 && !game.IsGameOver (); r++)
      {
        const int t = game.GetRoundIndex ();
        std::vector<Action> acts (2);
        for (int i = 0; i < 2; i++)
          {
            const int other = 1 - i;
            const int dx = game.GetPlayerX (other) - game.GetPlayerX (i);
            const int dy = game.GetPlayerY (other) - game.GetPlayerY (i);
            acts[i] = (dx == 0 && dy == 0)
                ? WaitAction ()
                : MoveAction (dx > 0 ? 1 : (dx < 0 ? -1 : 0),
                              dy > 0 ? 1 : (dy < 0 ? -1 : 0));
          }

        for (int i = 0; i < 2; i++)
          {
            const auto c = CommitAction (
                DuelCommitHash (1, t, i, acts[i], Salt (t * 2 + i)));
            EXPECT_TRUE (game.ProcessAction (i, c));
            log.push_back ({i, c});
          }
        for (int i = 0; i < 2; i++)
          {
            const auto rv = RevealAction (Salt (t * 2 + i));
            EXPECT_TRUE (game.ProcessAction (i, rv));
            log.push_back ({i, rv});
          }
        for (int i = 0; i < 2; i++)
          {
            if (game.IsGameOver () || !game.IsPlayerActive (i))
              continue;
            EXPECT_TRUE (game.ProcessAction (i, acts[i]));
            log.push_back ({i, acts[i]});
          }
      }
    return game;
  }

  /** The wire JSON for a merged log carrying duel entries.  */
  static std::string LogJson (const std::vector<LoggedAction>& entries)
  {
    Json::Value arr (Json::arrayValue);
    for (const auto& la : entries)
      {
        Json::Value a (Json::objectValue);
        a["i"] = la.actor;
        switch (la.action.type)
          {
          case Action::Type::Move:
            a["type"] = "move";
            a["dx"] = la.action.dx;
            a["dy"] = la.action.dy;
            break;
          case Action::Type::Wait:
            a["type"] = "wait";
            break;
          case Action::Type::UseItem:
            a["type"] = "use";
            a["item"] = la.action.itemId;
            break;
          case Action::Type::EnterGate:
            a["type"] = "gate";
            break;
          case Action::Type::Commit:
            a["type"] = "commit";
            a["h"] = la.action.hex;
            break;
          case Action::Type::Reveal:
            a["type"] = "reveal";
            a["s"] = la.action.hex;
            break;
          default:
            ADD_FAILURE () << "unexpected action in a duel log";
            break;
          }
        arr.append (a);
      }
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    return Json::writeString (wb, arr);
  }

  /** The claims a correct client computes for a finished duel.  */
  std::string ClaimsFor (const DungeonGame& game)
  {
    std::vector<int64_t> damages;
    for (int i = 0; i < game.GetPlayerCount (); i++)
      damages.push_back (game.GetDamageDealt (i));
    const auto xp = SplitPool (game.GetXpPool (), damages);
    const auto gold = SplitPool (game.GetKillGoldPool (), damages);
    const int winner = game.GetDuelWinner ();
    const int loser = winner == 0 ? 1 : 0;
    const char* names[] = {"alice", "bob"};

    const int64_t loserLevel = QueryInt (
      "SELECT `level` FROM `players` WHERE `name` = '"
      + std::string (names[loser]) + "'");
    const int64_t pot = QueryInt ("SELECT `pot` FROM `visits` WHERE `id` = 1");

    std::string out = "[";
    for (int i = 0; i < 2; i++)
      {
        if (i > 0)
          out += ",";
        const bool won = i == winner;
        out += R"({"p": ")" + std::string (names[i]) + R"(", "survived": )"
             + (won ? "true" : "false")
             + R"(, "xp": )"
             + std::to_string (xp[i]
                 + (won ? MoveProcessor::DUEL_XP_BASE * loserLevel : 0))
             + R"(, "gold": )"
             + std::to_string (game.GetTotalGold (i) + gold[i]
                               + (won ? pot : 0))
             + R"(, "kills": )" + std::to_string (game.GetTotalKills (i))
             + R"(, "duel": ")" + (won ? "won" : "lost") + R"("})";
      }
    return out + "]";
  }

  void Confirm (const std::string& name, const std::string& hash,
                const int64_t n, const unsigned height = 400)
  {
    ProcessMove (name,
                 R"({"sc": {"id": 1, "h": ")" + hash
                 + R"(", "n": )" + std::to_string (n) + "}}", height);
  }

  void Settle (const std::string& name, const std::string& claims,
               const std::string& actions, const unsigned height = 401,
               const int64_t soloFrom = -1)
  {
    const std::string solo = soloFrom < 0 ? ""
        : R"(, "solo_from": )" + std::to_string (soloFrom);
    ProcessMove (name,
                 R"({"s": {"id": 1, "results": )" + claims
                 + R"(, "actions": )" + actions + solo + "}}", height);
  }

};

TEST_F (DuelSettleTests, WinnerTakesThePot)
{
  auto game = FightToTheDeath ();
  ASSERT_TRUE (game.IsGameOver ());
  const int winner = game.GetDuelWinner ();
  ASSERT_GE (winner, 0);

  const char* names[] = {"alice", "bob"};
  const std::string winnerName = names[winner];
  const std::string loserName = names[winner == 0 ? 1 : 0];
  const int64_t loserLevel = QueryInt (
    "SELECT `level` FROM `players` WHERE `name` = '" + loserName + "'");
  const int64_t goldBefore = Gold (winnerName);

  const std::string claims = ClaimsFor (game);
  Confirm (loserName, SettleLogHash (1, log), log.size ());
  Settle (winnerName, claims, LogJson (log));

  EXPECT_EQ (QueryString ("SELECT `status` FROM `visits` WHERE `id` = 1"),
             "completed");

  /* The whole 60-gold pot went to the winner, and the escrow is spent.  */
  EXPECT_EQ (Gold (winnerName), goldBefore + 60);
  EXPECT_EQ (QueryInt ("SELECT `pot` FROM `visits` WHERE `id` = 1"), 0);

  /* Banked as survived without ever reaching a gate.  */
  EXPECT_EQ (QueryInt (
    "SELECT `survived` FROM `visit_results` WHERE `visit_id` = 1"
    " AND `name` = '" + winnerName + "'"), 1);
  EXPECT_EQ (QueryString (
    "SELECT `duel` FROM `visit_results` WHERE `visit_id` = 1"
    " AND `name` = '" + winnerName + "'"), "won");
  EXPECT_EQ (QueryString (
    "SELECT `duel` FROM `visit_results` WHERE `visit_id` = 1"
    " AND `name` = '" + loserName + "'"), "lost");

  /* The winner gained DUEL_XP_BASE * loserLevel on top of monster XP.  */
  EXPECT_EQ (QueryInt (
    "SELECT `xp_gained` FROM `visit_results` WHERE `visit_id` = 1"
    " AND `name` = '" + winnerName + "'"),
             MoveProcessor::DUEL_XP_BASE * loserLevel);

  /* The loser took the ordinary death outcome.  */
  EXPECT_EQ (QueryInt (
    "SELECT `deaths` FROM `players` WHERE `name` = '" + loserName + "'"), 1);
  EXPECT_EQ (QueryInt (
    "SELECT `survived` FROM `visit_results` WHERE `visit_id` = 1"
    " AND `name` = '" + loserName + "'"), 0);
}

TEST_F (DuelSettleTests, LoserKeepsNoMoreThanTheStakeAndTheDeathTax)
{
  auto game = FightToTheDeath ();
  const int winner = game.GetDuelWinner ();
  const char* names[] = {"alice", "bob"};
  const std::string winnerName = names[winner];
  const std::string loserName = names[winner == 0 ? 1 : 0];

  /* The stake already left when the duel was joined, so the death tax is
     charged on what is left, never on the escrow (spec section 5a).  */
  const int64_t loserGoldBefore = Gold (loserName);
  ASSERT_EQ (loserGoldBefore, 70);

  Confirm (loserName, SettleLogHash (1, log), log.size ());
  Settle (winnerName, ClaimsFor (game), LogJson (log));

  const int64_t earned = QueryInt (
    "SELECT `gold_gained` FROM `visit_results` WHERE `visit_id` = 1"
    " AND `name` = '" + loserName + "'");
  EXPECT_EQ (Gold (loserName), ((loserGoldBefore + earned) * 75) / 100);
}

TEST_F (DuelSettleTests, ClaimingTheWrongWinnerIsRejected)
{
  auto game = FightToTheDeath ();
  const int winner = game.GetDuelWinner ();
  const char* names[] = {"alice", "bob"};
  const std::string winnerName = names[winner];
  const std::string loserName = names[winner == 0 ? 1 : 0];

  /* Swap the duel outcomes in the claims and leave everything else
     alone.  The GSP recomputes the winner from the replay.  */
  std::string claims = ClaimsFor (game);
  const size_t first = claims.find (R"("duel": ")");
  ASSERT_NE (first, std::string::npos);
  const size_t second = claims.find (R"("duel": ")", first + 1);
  ASSERT_NE (second, std::string::npos);
  const bool firstWon = claims.compare (first + 9, 3, "won") == 0;
  claims = claims.substr (0, first) + R"("duel": ")"
      + (firstWon ? "lost" : "won") + R"("})"
      + claims.substr (claims.find ('}', first) + 1);

  Confirm (loserName, SettleLogHash (1, log), log.size ());
  Settle (winnerName, claims, LogJson (log));

  EXPECT_EQ (QueryString ("SELECT `status` FROM `visits` WHERE `id` = 1"),
             "active");
  EXPECT_EQ (QueryInt ("SELECT `pot` FROM `visits` WHERE `id` = 1"), 60);
}

TEST_F (DuelSettleTests, UndecidedDuelCannotBeSettled)
{
  auto game = BuildDuel ();
  log.clear ();

  /* Three rounds of waits: nobody has won, so there is nothing to
     settle and the pot stays in escrow.  */
  for (int r = 0; r < 3; r++)
    {
      const int t = game.GetRoundIndex ();
      for (int i = 0; i < 2; i++)
        {
          const auto c = CommitAction (
              DuelCommitHash (1, t, i, WaitAction (), Salt (t * 2 + i)));
          ASSERT_TRUE (game.ProcessAction (i, c));
          log.push_back ({i, c});
        }
      for (int i = 0; i < 2; i++)
        {
          const auto rv = RevealAction (Salt (t * 2 + i));
          ASSERT_TRUE (game.ProcessAction (i, rv));
          log.push_back ({i, rv});
        }
      for (int i = 0; i < 2; i++)
        {
          ASSERT_TRUE (game.ProcessAction (i, WaitAction ()));
          log.push_back ({i, WaitAction ()});
        }
    }
  ASSERT_EQ (game.GetDuelWinner (), -1);

  const std::string claims = R"([
    {"p": "alice", "survived": false, "xp": 0, "gold": 0, "kills": 0,
     "duel": "lost"},
    {"p": "bob", "survived": false, "xp": 0, "gold": 0, "kills": 0,
     "duel": "lost"}])";
  Confirm ("bob", SettleLogHash (1, log), log.size ());
  Settle ("alice", claims, LogJson (log));

  EXPECT_EQ (QueryString ("SELECT `status` FROM `visits` WHERE `id` = 1"),
             "active");
  EXPECT_EQ (QueryInt ("SELECT `pot` FROM `visits` WHERE `id` = 1"), 60);
}

TEST_F (DuelSettleTests, StallerLosesThroughAbandonment)
{
  /* Bob stops revealing after two rounds.  Alice waits out the window and
     settles from his last checkpoint: he is absent, so he loses and Alice
     takes the pot without ever landing a blow (spec section 7).  */
  auto game = BuildDuel ();
  log.clear ();
  for (int r = 0; r < 2; r++)
    {
      const int t = game.GetRoundIndex ();
      for (int i = 0; i < 2; i++)
        {
          const auto c = CommitAction (
              DuelCommitHash (1, t, i, WaitAction (), Salt (t * 2 + i)));
          ASSERT_TRUE (game.ProcessAction (i, c));
          log.push_back ({i, c});
        }
      for (int i = 0; i < 2; i++)
        {
          const auto rv = RevealAction (Salt (t * 2 + i));
          ASSERT_TRUE (game.ProcessAction (i, rv));
          log.push_back ({i, rv});
        }
      for (int i = 0; i < 2; i++)
        {
          ASSERT_TRUE (game.ProcessAction (i, WaitAction ()));
          log.push_back ({i, WaitAction ()});
        }
    }

  const int64_t aliceBefore = Gold ("alice");
  const int64_t bobLevel = QueryInt (
    "SELECT `level` FROM `players` WHERE `name` = 'bob'");

  Confirm ("bob", SettleLogHash (1, log), log.size (), 400);

  const std::string claims = R"([
    {"p": "alice", "survived": true, "xp": )"
    + std::to_string (MoveProcessor::DUEL_XP_BASE * bobLevel)
    + R"(, "gold": 60, "kills": 0, "duel": "won"},
    {"p": "bob", "survived": false, "xp": 0, "gold": 0, "kills": 0,
     "duel": "lost"}])";

  /* Too early: the checkpoint is still fresh, so bob might just be slow.  */
  Settle ("alice", claims, LogJson (log), 405, log.size ());
  EXPECT_EQ (QueryString ("SELECT `status` FROM `visits` WHERE `id` = 1"),
             "active");

  /* Past the window, the stall is indistinguishable from vanishing.  */
  Settle ("alice", claims, LogJson (log),
          400 + MoveProcessor::ABANDON_WINDOW_BLOCKS, log.size ());

  EXPECT_EQ (QueryString ("SELECT `status` FROM `visits` WHERE `id` = 1"),
             "completed");
  EXPECT_EQ (Gold ("alice"), aliceBefore + 60);
  EXPECT_EQ (QueryString (
    "SELECT `duel` FROM `visit_results` WHERE `visit_id` = 1"
    " AND `name` = 'bob'"), "lost");
  EXPECT_EQ (QueryInt (
    "SELECT `deaths` FROM `players` WHERE `name` = 'bob'"), 1);
}

/**
 * No side door out of a duel with gold in it.  A duellist is in an active
 * visit but NOT in a channel, so every solo escape route is already shut
 * by the existing guards; these lock that shut for duels specifically,
 * because a duellist who could walk away mid-fight would be walking away
 * from a stake.
 */
TEST_F (DuelSettleTests, NoSoloEscapeFromADuel)
{
  const int64_t goldBefore = Gold ("bob");

  /* Opening a private run instead of fighting.  */
  ProcessMove ("bob", R"({"ec": {"x": 1, "y": 0}})", 350);
  EXPECT_EQ (QueryInt ("SELECT COUNT(*) FROM `visits`"), 1);
  EXPECT_EQ (QueryInt (
    "SELECT `in_channel` FROM `players` WHERE `name` = 'bob'"), 0);

  /* Walking out through a gate, with or without claiming a transit.  */
  ProcessMove ("bob", R"({"gw": {"dir": "east", "transit": true}})", 351);
  ProcessMove ("bob", R"({"gw": {"dir": "east"}})", 352);
  EXPECT_EQ (PlayerSegment ("bob"), SegmentKey (0, 0));

  /* Settling the duel as though it were a solo run: `xc` is the
     initiator's move and needs a channel, which a duellist does not
     have.  */
  ProcessMove ("alice", R"({"xc": {"id": 1, "results": {"survived": true},
                                   "actions": []}})", 353);
  EXPECT_EQ (QueryString ("SELECT `status` FROM `visits` WHERE `id` = 1"),
             "active");

  /* The pot is untouched and nobody got their stake back the easy way.  */
  EXPECT_EQ (QueryInt ("SELECT `pot` FROM `visits` WHERE `id` = 1"), 60);
  EXPECT_EQ (Gold ("bob"), goldBefore);
}

TEST_F (DuelSettleTests, AbandonmentSettleCannotCarryASoloSuffix)
{
  /* An absent duellist has already lost, so there is nothing to play out
     alone: a suffix after the checkpoint is refused outright.  */
  auto game = BuildDuel ();
  log.clear ();
  const int t = game.GetRoundIndex ();
  for (int i = 0; i < 2; i++)
    {
      const auto c = CommitAction (
          DuelCommitHash (1, t, i, WaitAction (), Salt (i)));
      ASSERT_TRUE (game.ProcessAction (i, c));
      log.push_back ({i, c});
    }
  for (int i = 0; i < 2; i++)
    {
      const auto rv = RevealAction (Salt (i));
      ASSERT_TRUE (game.ProcessAction (i, rv));
      log.push_back ({i, rv});
    }
  for (int i = 0; i < 2; i++)
    {
      ASSERT_TRUE (game.ProcessAction (i, WaitAction ()));
      log.push_back ({i, WaitAction ()});
    }

  auto withSuffix = log;
  withSuffix.push_back ({0, CommitAction (std::string (64, 'a'))});

  Confirm ("bob", SettleLogHash (1, log), log.size (), 400);
  Settle ("alice", R"([
    {"p": "alice", "survived": true, "xp": 0, "gold": 60, "kills": 0,
     "duel": "won"},
    {"p": "bob", "survived": false, "xp": 0, "gold": 0, "kills": 0,
     "duel": "lost"}])",
          LogJson (withSuffix),
          400 + MoveProcessor::ABANDON_WINDOW_BLOCKS, log.size ());

  EXPECT_EQ (QueryString ("SELECT `status` FROM `visits` WHERE `id` = 1"),
             "active");
  EXPECT_EQ (QueryInt ("SELECT `pot` FROM `visits` WHERE `id` = 1"), 60);
}

/* ************************************************************************ */

/**
 * The survival heal, scaled by how much of the segment the run cleared
 * (checklist item 21).  Pure integer math on a formula every node has to
 * agree on, so it is worth pinning the curve rather than only its ends.
 */
TEST (SurvivalHealTests, ScalesWithClearance)
{
  const int64_t full = MoveProcessor::SURVIVAL_HEAL_PERCENT;

  /* Nothing cleared pays nothing: stepping straight back out of the gate
     you came in by is no longer a heal button.  */
  EXPECT_EQ (SurvivalHealPercent (0, 12), 0);

  /* Three quarters pays the full heal, and so does anything above it.  */
  EXPECT_EQ (SurvivalHealPercent (9, 12), full);
  EXPECT_EQ (SurvivalHealPercent (10, 12), full);
  EXPECT_EQ (SurvivalHealPercent (12, 12), full);

  /* Below it, a smooth scale rather than a cliff.  Half the monsters is
     two thirds of the way to the three-quarter threshold, so two thirds
     of the heal.  */
  EXPECT_EQ (SurvivalHealPercent (6, 12), full * 2 / 3);
  EXPECT_EQ (SurvivalHealPercent (3, 12), full / 3);

  /* Monotonic: one more kill never pays less.  */
  for (int k = 0; k < 12; k++)
    EXPECT_LE (SurvivalHealPercent (k, 12), SurvivalHealPercent (k + 1, 12));

  /* A visit the spawn cull emptied has nothing to fight, so it counts as
     cleared rather than punishing the player for it.  */
  EXPECT_EQ (SurvivalHealPercent (0, 0), full);
}

TEST_F (DuelSettleTests, DuelWinTakesNoSurvivalHeal)
{
  auto game = FightToTheDeath ();
  const int winner = game.GetDuelWinner ();
  const char* names[] = {"alice", "bob"};
  const std::string winnerName = names[winner];
  const std::string loserName = names[winner == 0 ? 1 : 0];

  Confirm (loserName, SettleLogHash (1, log), log.size ());
  Settle (winnerName, ClaimsFor (game), LogJson (log));
  ASSERT_EQ (QueryString ("SELECT `status` FROM `visits` WHERE `id` = 1"),
             "completed");

  /* Banked at exactly the HP the replay left them on: the heal is
     exploration sustain for a surviving gate-walk, and a duel winner never
     walks through a gate (pvp spec section 5).  */
  const int64_t replayHp = QueryInt (
    "SELECT `hp_remaining` FROM `visit_results` WHERE `visit_id` = 1"
    " AND `name` = '" + winnerName + "'");
  EXPECT_EQ (QueryInt (
    "SELECT `hp` FROM `players` WHERE `name` = '" + winnerName + "'"),
             replayHp);
}

} // anonymous namespace
} // namespace rog
