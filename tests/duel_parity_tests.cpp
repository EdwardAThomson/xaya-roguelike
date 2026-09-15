/**
 * Cross-language parity vectors for the duel engine and its round
 * protocol (SPEC_multiplayer_pvp.md section 10).  The TypeScript frontend
 * replays the IDENTICAL pinned inputs; the canonical summary lines printed
 * here must match the frontend's byte-for-byte, and the baked expectations
 * below guard against a silent drift on this side alone.
 *
 * The fixtures pin the SALTS and the ACTIONS; the commitments are derived
 * from them through DuelCommitHash, so the commitment encoding is covered
 * too -- a commitment built differently on the other side produces
 * different commit entries, a different settle-log hash, and a failing
 * PARITY line.
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

/** Parses one canonical action body ("move 1 0", "use health_potion").  */
Action
ParseActionBody (const std::string& body)
{
  std::istringstream in (body);
  std::string type;
  in >> type;

  Action a;
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
  else if (type == "commit")
    {
      a.type = Action::Type::Commit;
      in >> a.hex;
    }
  else if (type == "reveal")
    {
      a.type = Action::Type::Reveal;
      in >> a.hex;
    }
  else
    ADD_FAILURE () << "bad canonical action body: " << body;
  return a;
}

/** One participant's choice for a round: what they do and the salt that
    seals it.  */
struct Choice
{
  Action action;
  std::string salt;
};

/** A round's choices, in canonical order of the round's ACTIVE
    participants (which is the order the round protocol itself runs in).  */
using Round = std::vector<Choice>;

/** Parses "move 0 -1,<salt>|use health_potion,<salt>" into one round.  */
Round
ParseRound (const std::string& row)
{
  Round round;
  size_t pos = 0;
  while (pos <= row.size ())
    {
      const size_t bar = row.find ('|', pos);
      const std::string entry = row.substr (
          pos, bar == std::string::npos ? std::string::npos : bar - pos);
      pos = bar == std::string::npos ? row.size () + 1 : bar + 1;
      if (entry.empty ())
        continue;

      const size_t comma = entry.rfind (',');
      if (comma == std::string::npos)
        {
          ADD_FAILURE () << "bad duel fixture row entry: " << entry;
          continue;
        }
      Choice c;
      c.action = ParseActionBody (entry.substr (0, comma));
      c.salt = entry.substr (comma + 1);
      round.push_back (c);
    }
  return round;
}

std::vector<Round>
ParseRounds (const std::vector<std::string>& rows)
{
  std::vector<Round> out;
  for (const auto& r : rows)
    out.push_back (ParseRound (r));
  return out;
}

/**
 * Drives a duel through the pinned rounds exactly as a client would: every
 * active participant commits, then every one reveals, then every one acts.
 * Appends the merged log it produces -- commit and reveal entries included
 * (spec section 6) -- to `log`.
 */
void
DriveDuel (DungeonGame& game, const int64_t visitId,
           const std::vector<Round>& rounds, std::vector<LoggedAction>& log)
{
  for (const auto& round : rounds)
    {
      if (game.IsGameOver ())
        break;

      const int t = game.GetRoundIndex ();
      std::vector<int> actors;
      for (int i = 0; i < game.GetPlayerCount (); i++)
        if (game.IsPlayerActive (i))
          actors.push_back (i);
      ASSERT_EQ (round.size (), actors.size ())
          << "fixture round " << t << " does not cover the active set";

      ASSERT_EQ (game.GetPhase (), DungeonGame::Phase::Commit);
      for (size_t k = 0; k < actors.size (); k++)
        {
          Action c;
          c.type = Action::Type::Commit;
          c.hex = DuelCommitHash (visitId, t, actors[k], round[k].action,
                                  round[k].salt);
          ASSERT_TRUE (game.ProcessAction (actors[k], c))
              << "commit rejected in round " << t;
          log.push_back ({actors[k], c});
        }

      ASSERT_EQ (game.GetPhase (), DungeonGame::Phase::Reveal);
      for (size_t k = 0; k < actors.size (); k++)
        {
          Action r;
          r.type = Action::Type::Reveal;
          r.hex = round[k].salt;
          ASSERT_TRUE (game.ProcessAction (actors[k], r))
              << "reveal rejected in round " << t;
          log.push_back ({actors[k], r});
        }

      ASSERT_EQ (game.GetPhase (), DungeonGame::Phase::Act);
      for (size_t k = 0; k < actors.size (); k++)
        {
          /* A participant killed earlier in this round's application takes
             no action, and neither does anyone once the duel is decided
             (spec section 2).  */
          if (game.IsGameOver () || !game.IsPlayerActive (actors[k]))
            continue;
          ASSERT_TRUE (game.ProcessAction (actors[k], round[k].action))
              << "action rejected in round " << t;
          log.push_back ({actors[k], round[k].action});
        }
    }
}

/* The duellists.  Deliberately unequal (level, stats, HP) so an asymmetry
   in either engine's stat handling shows up in the outcome.  */
std::vector<DungeonGame::PlayerSetup>
DuelFixtureSetups ()
{
  DungeonGame::PlayerSetup alice;
  alice.stats.level = 3;
  alice.stats.strength = 13;
  alice.stats.dexterity = 11;
  alice.stats.constitution = 10;
  alice.stats.intelligence = 10;
  alice.stats.equipAttack = 5;
  alice.stats.equipDefense = 2;
  alice.hp = 100;
  alice.maxHp = 100;
  alice.potions = {{"health_potion", 1}};
  alice.inventory = {
    {1, "short_sword", "weapon"},
    {2, "leather_armor", "body"},
  };
  alice.entryDir = "south";

  DungeonGame::PlayerSetup bob;
  bob.stats.level = 2;
  bob.stats.strength = 12;
  bob.stats.dexterity = 9;
  bob.stats.constitution = 11;
  bob.stats.intelligence = 9;
  bob.stats.equipAttack = 5;
  bob.stats.equipDefense = 2;
  bob.hp = 95;
  bob.maxHp = 105;
  bob.potions = {{"health_potion", 1}};
  bob.inventory = {
    {11, "short_sword", "weapon"},
    {12, "leather_armor", "body"},
  };
  bob.entryDir = "south";

  return {alice, bob};
}

constexpr const char* DUEL_FIXTURE_SEED = "duel-parity-1";
constexpr int DUEL_FIXTURE_DEPTH = 3;
constexpr int64_t DUEL_FIXTURE_VISIT_ID = 11;

/* A fought-out duel: both walk in through the SAME gate, so they spawn one
   tile apart and trade blows every round.  Generated once by a scripted
   mutual-attack policy searched over salt nonces until the run covered
   every player-vs-player outcome the spec requires a vector for -- a hit,
   a miss, a dodge and a critical -- and ended in a death (section 10).
   Round 3 drinks participant 1's only potion; round 4 commits to drinking
   another it no longer holds, which the duel applies as a wait while the
   log keeps the action the commitment covers (section 2).  */
const std::vector<std::string> DUEL_FIXTURE_ROUNDS = {
  "move 0 -1,46fe29bd55091bac1f585e9361040b60|move 0 1,505b34f6f400bb66c1e6575e3381ae98",
  "move 0 -1,24a4f60012bc28931beaf295e8afc0d5|move 0 1,f30ac68cf470f7aa149551476f12fe59",
  "move 0 -1,b6fb3ceade09780b8cbe23823372c8da|move 0 1,4828070ebb765574dc2421c459fbfee3",
  "move 0 -1,d1be09e4055c76bf0f0c62408908a51e|use health_potion,2d75ca6c55176aaf48d360498dc64c22",
  "move 0 -1,2a09226e39f3009bb3f3a3cede8c5d76|use health_potion,144f587a3222c320763b64abdada5753",
  "move 0 -1,c56982aa6abb747bc31a9952393698fc|move 0 1,e02ce7a7cc372690d870c5429abc8db4",
  "move 0 -1,1900b6e92fb00dee570004971afa8eba|move 0 1,28426d0d232c3f62d536cc7e79f6c1e2",
  "move 0 -1,417bd4cd0528ae7ac0b085aaf9b22feb|move 0 1,ddc0231256211c53de48480cf21e0b0d",
  "move 0 -1,1a24b03a4ae3f9e69c3c21e5436c89b0|move 0 1,62430b8ec12b2c2c7b8cbb154cd294aa",
  "move 0 -1,8a962e81c7ee23d1321eb8abe87f0fee|move 0 1,5a3d1d71d010a2ab19230fa04f706c0b",
  "move 0 -1,ec759f88c6c13b83f843e4ef88198a22|move 0 1,9713c120118acda4aebcd908583ed8fb",
  "move 0 -1,2fcc9a2378f4d83813354b92a76cc9aa|move 0 1,a18f8a943e79ef26e4138a1073cd7a50",
  "move 0 -1,f3b27594550b419515aa46eea5a8bc08|move 0 1,449f379e39f4f25a2f90849248297619",
  "move 0 -1,5d7ae7e5edd453caf4c60b2eb1ddf9f6|move 0 1,e2176b074dc1ca94edcb10bf9f6e5a49",
};

/** The canonical summary line, diffed byte-for-byte against the frontend. */
std::string
DuelParityLine (const char* tag, const DungeonGame& game,
                const std::vector<LoggedAction>& log)
{
  std::vector<int64_t> damages;
  for (int i = 0; i < game.GetPlayerCount (); i++)
    damages.push_back (game.GetDamageDealt (i));
  const auto xpShares = SplitPool (game.GetXpPool (), damages);
  const auto goldShares = SplitPool (game.GetKillGoldPool (), damages);

  std::string line = tag;
  for (int i = 0; i < game.GetPlayerCount (); i++)
    {
      char buf[256];
      std::snprintf (buf, sizeof (buf),
                     " p%d[dead=%d exited=%d absent=%d xp=%lld gold=%lld"
                     " kills=%d hp=%d dmg=%d pvp=%d death=%d exit=%s]",
                     i, game.IsPlayerDead (i) ? 1 : 0,
                     game.HasPlayerExited (i) ? 1 : 0,
                     game.IsPlayerAbsent (i) ? 1 : 0,
                     static_cast<long long> (xpShares[i]),
                     static_cast<long long> (game.GetTotalGold (i)
                                             + goldShares[i]),
                     game.GetTotalKills (i), game.GetPlayerHp (i),
                     game.GetDamageDealt (i), game.GetPvpDamage (i),
                     game.GetDeathSeq (i), game.GetExitGate (i).c_str ());
      line += buf;
    }
  line += " winner=" + std::to_string (game.GetDuelWinner ());
  line += " rounds=" + std::to_string (game.GetRoundIndex ());
  line += " entries=" + std::to_string (log.size ());
  line += " hash=" + SettleLogHash (DUEL_FIXTURE_VISIT_ID, log);
  return line;
}

TEST (DuelParityTests, FoughtOutDuelVector)
{
  const auto rounds = ParseRounds (DUEL_FIXTURE_ROUNDS);
  auto game = DungeonGame::CreateDuel (DUEL_FIXTURE_SEED, DUEL_FIXTURE_DEPTH,
                                        DuelFixtureSetups (),
                                        DUEL_FIXTURE_VISIT_ID);

  /* Both walk in through the south gate, so the ring scan of the co-op
     spec section 2a puts them one tile apart: a duel starts in contact.  */
  ASSERT_EQ (game.GetPlayerX (0), 56);
  ASSERT_EQ (game.GetPlayerY (0), 38);
  ASSERT_EQ (game.GetPlayerX (1), 56);
  ASSERT_EQ (game.GetPlayerY (1), 37);

  std::vector<LoggedAction> log;
  DriveDuel (game, DUEL_FIXTURE_VISIT_ID, rounds, log);

  ASSERT_TRUE (game.IsGameOver ());
  ASSERT_EQ (game.GetMergedLog ().size (), log.size ());

  /* The settlement path replays the log alone; it must land on exactly
     the same state the live run did.  */
  auto replay = DungeonGame::ReplayDuel (DUEL_FIXTURE_SEED,
                                          DUEL_FIXTURE_DEPTH,
                                          DuelFixtureSetups (),
                                          DUEL_FIXTURE_VISIT_ID, log);
  ASSERT_EQ (replay.GetMergedLog ().size (), log.size ());

  const std::string line = DuelParityLine ("PARITY-DUEL", game, log);
  std::printf ("%s\n", line.c_str ());
  EXPECT_EQ (line, DuelParityLine ("PARITY-DUEL", replay, log));

  EXPECT_EQ (line,
             "PARITY-DUEL"
             " p0[dead=1 exited=0 absent=0 xp=0 gold=0 kills=0 hp=0 dmg=0"
             " pvp=107 death=1 exit=]"
             " p1[dead=0 exited=0 absent=0 xp=0 gold=0 kills=0 hp=17 dmg=0"
             " pvp=100 death=0 exit=]"
             " winner=1 rounds=13 entries=84"
             " hash=63712921fdf060e8cb3c29a4a9c14ff2e4b34a9eb2a538885a3518beb647448a");
}

/* Concession: participant 0 steps onto the gate it walked in through and
   leaves.  Exiting a duel is a forfeit, not an escape -- the opponent wins
   on the spot (spec section 5) -- so this vector pins the one outcome that
   reads the opposite way round from every other run in the game.  */
const std::vector<std::string> DUEL_CONCEDE_ROUNDS = {
  "move 0 1,00112233445566778899aabbccddeeff|move 0 1,ffeeddccbbaa99887766554433221100",
  "gate,0123456789abcdef0123456789abcdef|wait,fedcba9876543210fedcba9876543210",
};

TEST (DuelParityTests, ConcessionVector)
{
  const auto rounds = ParseRounds (DUEL_CONCEDE_ROUNDS);
  auto game = DungeonGame::CreateDuel (DUEL_FIXTURE_SEED, DUEL_FIXTURE_DEPTH,
                                        DuelFixtureSetups (),
                                        DUEL_FIXTURE_VISIT_ID);
  std::vector<LoggedAction> log;
  DriveDuel (game, DUEL_FIXTURE_VISIT_ID, rounds, log);

  /* The conceder walked out through a gate, so `exited` is true -- and the
     duel is still lost.  Settlement banks them as not survived (section
     5a), which is why the winner is read from GetDuelWinner and never from
     HasPlayerExited.  */
  EXPECT_TRUE (game.HasPlayerExited (0));
  EXPECT_EQ (game.GetExitGate (0), "south");
  EXPECT_EQ (game.GetDuelWinner (), 1);
  EXPECT_TRUE (game.IsGameOver ());

  /* The duel ended on participant 0's gate action, so participant 1 never
     took its turn that round: 2 commits + 2 reveals + 2 actions, then
     2 commits + 2 reveals + 1 action.  */
  EXPECT_EQ (log.size (), 11u);

  const std::string line = DuelParityLine ("PARITY-DUEL-CONCEDE", game, log);
  std::printf ("%s\n", line.c_str ());
  EXPECT_EQ (line,
             "PARITY-DUEL-CONCEDE"
             " p0[dead=0 exited=1 absent=0 xp=0 gold=0 kills=0 hp=100 dmg=0"
             " pvp=0 death=0 exit=south]"
             " p1[dead=0 exited=0 absent=0 xp=0 gold=0 kills=0 hp=95 dmg=0"
             " pvp=0 death=0 exit=]"
             " winner=1 rounds=1 entries=11"
             " hash=067c1503278ac61ed7a14bceb315bc65440a4e176a361cecf0e70a4188efaaf2");
}

/* Refusal to reveal (spec section 7).  A duellist who dislikes the round
   it can already see can only stall, and stalling is indistinguishable
   from vanishing: the opponent settles from the last checkpoint with the
   staller marked absent, and an absent duellist loses.  The unrevealed
   round is simply not in the settled log, which is why the prefix ends on
   a round boundary.  */
constexpr int STALL_PREFIX_ROUNDS = 5;

TEST (DuelParityTests, RefusalToRevealVector)
{
  const auto rounds = ParseRounds (DUEL_FIXTURE_ROUNDS);
  const std::vector<Round> prefix (rounds.begin (),
                                   rounds.begin () + STALL_PREFIX_ROUNDS);

  auto game = DungeonGame::CreateDuel (DUEL_FIXTURE_SEED, DUEL_FIXTURE_DEPTH,
                                        DuelFixtureSetups (),
                                        DUEL_FIXTURE_VISIT_ID);
  std::vector<LoggedAction> log;
  DriveDuel (game, DUEL_FIXTURE_VISIT_ID, prefix, log);

  ASSERT_FALSE (game.IsGameOver ());
  ASSERT_EQ (game.GetDuelWinner (), -1);
  ASSERT_EQ (game.GetPhase (), DungeonGame::Phase::Commit);

  /* Participant 1 stops revealing; participant 0 settles past the window.  */
  game.MarkAbsent (1);
  EXPECT_TRUE (game.IsGameOver ());
  EXPECT_EQ (game.GetDuelWinner (), 0);

  const std::string line = DuelParityLine ("PARITY-DUEL-STALL", game, log);
  std::printf ("%s\n", line.c_str ());
  EXPECT_EQ (line,
             "PARITY-DUEL-STALL"
             " p0[dead=0 exited=0 absent=0 xp=0 gold=0 kills=0 hp=80 dmg=0"
             " pvp=27 death=0 exit=]"
             " p1[dead=0 exited=0 absent=1 xp=0 gold=0 kills=0 hp=97 dmg=0"
             " pvp=20 death=0 exit=]"
             " winner=0 rounds=5 entries=30"
             " hash=89297c9ba2febf01df5ba39fae36c835cb589d17d44a10df0a1359aa6fa4f510");
}

/**
 * The commitment preimage, pinned literally.  Every other duel vector
 * depends on this string, so pinning it here makes a cross-language
 * mismatch report itself directly instead of as a settle-hash difference.
 */
TEST (DuelParityTests, CommitHashVector)
{
  Action move;
  move.type = Action::Type::Move;
  move.dx = 1;
  move.dy = -1;

  const std::string salt = "00112233445566778899aabbccddeeff";
  const std::string preimage = DuelCommitPreimage (11, 7, 1, move, salt);
  EXPECT_EQ (preimage,
             "rog-duel-commit-v1\n11\n7\n1\nmove 1 -1\n"
             "00112233445566778899aabbccddeeff");

  const std::string h = DuelCommitHash (11, 7, 1, move, salt);
  std::printf ("PARITY-DUEL-COMMIT %s\n", h.c_str ());
  EXPECT_EQ (h,
             "08b987326245a8e68dd716b02f79a801812cc7db9a7069b0b835001b07f445f3");
}

/**
 * The settle-log encoding with the two duel entry kinds in it (spec
 * section 6): "<i> commit <h>" and "<i> reveal <s>".
 */
TEST (DuelParityTests, SettleLogHashWithDuelEntries)
{
  std::vector<LoggedAction> log;
  Action commit;
  commit.type = Action::Type::Commit;
  commit.hex = std::string (64, 'a');
  Action reveal;
  reveal.type = Action::Type::Reveal;
  reveal.hex = std::string (32, 'b');
  Action move;
  move.type = Action::Type::Move;
  move.dx = 0;
  move.dy = 1;
  Action wait;
  wait.type = Action::Type::Wait;

  log.push_back ({0, commit});
  log.push_back ({1, commit});
  log.push_back ({0, reveal});
  log.push_back ({1, reveal});
  log.push_back ({0, move});
  log.push_back ({1, wait});

  EXPECT_EQ (CanonicalActionLine (0, commit),
             "0 commit " + std::string (64, 'a') + "\n");
  EXPECT_EQ (CanonicalActionLine (1, reveal),
             "1 reveal " + std::string (32, 'b') + "\n");

  const std::string h = SettleLogHash (11, log);
  std::printf ("PARITY-DUEL-SETTLEHASH %s\n", h.c_str ());
  EXPECT_EQ (h,
             "6756cb0830f2e74460a9459d09acda930a685469903aaed3ac384c3b9c6b247f");
}

/** The compact encoding's duel codes, c<h> and r<s>.  */
TEST (DuelParityTests, CompactDuelEntries)
{
  std::vector<LoggedAction> out;
  const std::string compact =
      "0:c" + std::string (64, 'a') + ";1:c" + std::string (64, 'a')
      + ";0:r" + std::string (32, 'b') + ";1:r" + std::string (32, 'b')
      + ";0:m2;1:w";
  ASSERT_TRUE (ParseCompactActions (compact, true, out));
  ASSERT_EQ (out.size (), 6u);

  EXPECT_EQ (out[0].actor, 0);
  EXPECT_EQ (out[0].action.type, Action::Type::Commit);
  EXPECT_EQ (out[0].action.hex, std::string (64, 'a'));
  EXPECT_EQ (out[2].action.type, Action::Type::Reveal);
  EXPECT_EQ (out[2].action.hex, std::string (32, 'b'));
  EXPECT_EQ (out[4].action.type, Action::Type::Move);
  EXPECT_EQ (out[4].action.dy, 1);

  /* The compact form expands before anything else sees the log, so it
     hashes exactly as the verbose form does.  */
  std::vector<LoggedAction> verbose;
  Action commit;
  commit.type = Action::Type::Commit;
  commit.hex = std::string (64, 'a');
  Action reveal;
  reveal.type = Action::Type::Reveal;
  reveal.hex = std::string (32, 'b');
  Action move;
  move.type = Action::Type::Move;
  move.dx = 0;
  move.dy = 1;
  Action wait;
  wait.type = Action::Type::Wait;
  verbose.push_back ({0, commit});
  verbose.push_back ({1, commit});
  verbose.push_back ({0, reveal});
  verbose.push_back ({1, reveal});
  verbose.push_back ({0, move});
  verbose.push_back ({1, wait});
  EXPECT_EQ (SettleLogHash (11, out), SettleLogHash (11, verbose));
}

} // anonymous namespace
} // namespace rog
