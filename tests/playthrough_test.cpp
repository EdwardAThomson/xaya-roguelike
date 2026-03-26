#include "dungeongame.hpp"
#include "combat.hpp"
#include "dungeon.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <iostream>
#include <map>
#include <queue>
#include <sstream>
#include <string>

namespace rog
{
namespace
{

/**
 * A playthrough test that creates a dungeon, navigates it with simple AI,
 * and reports what happened.  This is a gameplay validation test, not a
 * correctness test — it exercises the full game loop.
 */
class PlaythroughTest : public testing::Test
{

protected:

  std::ostringstream log;

  PlayerStats MakeStats ()
  {
    PlayerStats s;
    s.level = 3;
    s.strength = 12;
    s.dexterity = 12;
    s.constitution = 12;
    s.intelligence = 10;
    s.equipAttack = 5;
    s.equipDefense = 3;
    return s;
  }

  Action MoveToward (int fromX, int fromY, int toX, int toY)
  {
    Action a;
    a.type = Action::Type::Move;
    a.dx = 0;
    a.dy = 0;
    if (toX > fromX) a.dx = 1;
    else if (toX < fromX) a.dx = -1;
    if (toY > fromY) a.dy = 1;
    else if (toY < fromY) a.dy = -1;
    return a;
  }

  /**
   * BFS pathfinding: returns the first step (dx, dy) on the shortest
   * path from (fromX,fromY) to (toX,toY), navigating around walls.
   * Returns (0,0) if no path exists.
   */
  std::pair<int, int> BfsNextStep (const DungeonGame& game,
                                    int fromX, int fromY,
                                    int toX, int toY)
  {
    using Pos = std::pair<int, int>;
    const auto& dungeon = game.GetDungeon ();

    std::queue<Pos> queue;
    std::map<Pos, Pos> parent;

    Pos start = {fromX, fromY};
    Pos goal = {toX, toY};
    queue.push (start);
    parent[start] = {-1, -1};

    static const int dx8[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    static const int dy8[] = {-1, 0, 1, -1, 1, -1, 0, 1};

    while (!queue.empty ())
      {
        auto [cx, cy] = queue.front ();
        queue.pop ();

        if (cx == toX && cy == toY)
          {
            /* Trace back to find the first step.  */
            Pos cur = goal;
            while (parent[cur] != start)
              cur = parent[cur];
            return {cur.first - fromX, cur.second - fromY};
          }

        for (int i = 0; i < 8; i++)
          {
            int nx = cx + dx8[i];
            int ny = cy + dy8[i];
            Pos next = {nx, ny};

            if (nx < 0 || nx >= Dungeon::WIDTH
                || ny < 0 || ny >= Dungeon::HEIGHT)
              continue;
            if (dungeon.GetTile (nx, ny) == Tile::Wall)
              continue;
            if (parent.count (next))
              continue;

            parent[next] = {cx, cy};
            queue.push (next);
          }
      }

    return {0, 0};  /* No path.  */
  }

  /** Renders the dungeon state as ASCII for debugging.  */
  std::string RenderMap (const DungeonGame& game, int viewRadius = 15)
  {
    std::ostringstream out;
    const int px = game.GetPlayerX ();
    const int py = game.GetPlayerY ();

    const int minX = std::max (0, px - viewRadius);
    const int maxX = std::min (Dungeon::WIDTH - 1, px + viewRadius);
    const int minY = std::max (0, py - viewRadius);
    const int maxY = std::min (Dungeon::HEIGHT - 1, py + viewRadius);

    for (int y = minY; y <= maxY; y++)
      {
        for (int x = minX; x <= maxX; x++)
          {
            if (x == px && y == py)
              {
                out << '@';
                continue;
              }

            /* Check for monster.  */
            bool isMonster = false;
            for (const auto& m : game.GetMonsters ())
              if (m.alive && m.x == x && m.y == y)
                {
                  out << m.symbol[0];
                  isMonster = true;
                  break;
                }
            if (isMonster)
              continue;

            /* Check for item.  */
            bool isItem = false;
            for (const auto& item : game.GetGroundItems ())
              if (item.x == x && item.y == y)
                {
                  out << '!';
                  isItem = true;
                  break;
                }
            if (isItem)
              continue;

            const Tile t = game.GetDungeon ().GetTile (x, y);
            switch (t)
              {
              case Tile::Wall: out << '#'; break;
              case Tile::Floor: out << '.'; break;
              case Tile::Gate: out << '+'; break;
              }
          }
        out << '\n';
      }
    return out.str ();
  }

};

TEST_F (PlaythroughTest, SurviveAndExit)
{
  const auto stats = MakeStats ();
  auto game = DungeonGame::Create ("playthrough_v1", 1, stats, 110, 110);

  log << "=== DUNGEON PLAYTHROUGH ===\n";
  log << "Seed: playthrough_v1, Depth: 1\n";
  log << "Player at (" << game.GetPlayerX () << ", " << game.GetPlayerY ()
      << ") HP: " << game.GetPlayerHp () << "/" << game.GetPlayerMaxHp () << "\n";
  log << "Monsters: " << game.GetMonsters ().size () << "\n";
  log << "Items on ground: " << game.GetGroundItems ().size () << "\n\n";

  /* List monsters.  */
  for (const auto& m : game.GetMonsters ())
    log << "  " << m.name << " at (" << m.x << ", " << m.y
        << ") HP:" << m.hp << " ATK:" << m.attack << "\n";
  log << "\n";

  /* Strategy: head toward the nearest gate, fighting anything in the way.
     If HP drops below 30%, try to use a health potion from loot.  */

  const auto& gates = game.GetDungeon ().GetGates ();
  ASSERT_FALSE (gates.empty ());

  /* Pick a gate target.  */
  int targetGateIdx = 0;
  int bestGateDist = 9999;
  for (size_t i = 0; i < gates.size (); i++)
    {
      const int d = std::abs (gates[i].x - game.GetPlayerX ())
                  + std::abs (gates[i].y - game.GetPlayerY ());
      if (d < bestGateDist)
        {
          bestGateDist = d;
          targetGateIdx = static_cast<int> (i);
        }
    }

  const int gateX = gates[targetGateIdx].x;
  const int gateY = gates[targetGateIdx].y;
  log << "Target gate: " << gates[targetGateIdx].direction
      << " at (" << gateX << ", " << gateY << ") dist=" << bestGateDist << "\n\n";

  const int MAX_TURNS = 500;

  for (int turn = 0; turn < MAX_TURNS && !game.IsGameOver (); turn++)
    {
      const int px = game.GetPlayerX ();
      const int py = game.GetPlayerY ();
      const int hp = game.GetPlayerHp ();

      /* Use health potion if HP below 30%.  */
      if (hp < game.GetPlayerMaxHp () * 30 / 100)
        {
          Action usePotion;
          usePotion.type = Action::Type::UseItem;
          usePotion.itemId = "health_potion";
          if (game.ProcessAction (usePotion))
            {
              log << "Turn " << turn << ": Used health potion! HP now "
                  << game.GetPlayerHp () << "\n";
              continue;
            }
        }

      /* If on the gate, enter it.  */
      if (px == gateX && py == gateY)
        {
          log << "Turn " << turn << ": At the gate! Entering...\n";
          game.ProcessAction ({Action::Type::EnterGate});
          break;
        }

      /* If standing on an item, pick it up.  */
      bool pickedUp = false;
      for (const auto& item : game.GetGroundItems ())
        if (item.x == px && item.y == py)
          {
            if (game.ProcessAction ({Action::Type::Pickup}))
              {
                log << "Turn " << turn << ": Picked up " << item.itemId
                    << " x" << item.quantity << "\n";
                pickedUp = true;
                break;
              }
          }
      if (pickedUp)
        continue;

      /* If a monster is adjacent, move into it to attack.  */
      bool attacked = false;
      for (const auto& m : game.GetMonsters ())
        {
          if (!m.alive)
            continue;
          if (std::abs (m.x - px) <= 1 && std::abs (m.y - py) <= 1)
            {
              auto atkAct = MoveToward (px, py, m.x, m.y);
              if (game.ProcessAction (atkAct))
                {
                  if (!m.alive)
                    log << "Turn " << turn << ": Killed " << m.name << "!\n";
                  attacked = true;
                  break;
                }
            }
        }
      if (attacked)
        continue;

      /* BFS pathfind toward gate.  */
      auto [stepX, stepY] = BfsNextStep (game, px, py, gateX, gateY);
      if (stepX != 0 || stepY != 0)
        {
          Action moveAct;
          moveAct.type = Action::Type::Move;
          moveAct.dx = stepX;
          moveAct.dy = stepY;
          if (!game.ProcessAction (moveAct))
            game.ProcessAction ({Action::Type::Wait});
        }
      else
        {
          /* No path found — wait.  */
          game.ProcessAction ({Action::Type::Wait});
        }

      /* Log interesting events every 50 turns or on damage.  */
      if (turn % 50 == 0 || game.GetPlayerHp () != hp)
        {
          log << "Turn " << turn << ": pos=(" << game.GetPlayerX ()
              << "," << game.GetPlayerY () << ") HP="
              << game.GetPlayerHp () << "/" << game.GetPlayerMaxHp ()
              << " kills=" << game.GetTotalKills ()
              << " gold=" << game.GetTotalGold () << "\n";
        }
    }

  /* Final report.  */
  log << "\n=== RESULT ===\n";
  log << "Turns taken: " << game.GetTurnCount () << "\n";
  log << "Game over: " << (game.IsGameOver () ? "yes" : "no") << "\n";
  log << "Survived: " << (game.HasSurvived () ? "yes" : "no") << "\n";
  log << "Exit gate: " << (game.GetExitGate ().empty () ? "(none)" : game.GetExitGate ()) << "\n";
  log << "Final HP: " << game.GetPlayerHp () << "/" << game.GetPlayerMaxHp () << "\n";
  log << "Kills: " << game.GetTotalKills () << "\n";
  log << "XP: " << game.GetTotalXp () << "\n";
  log << "Gold: " << game.GetTotalGold () << "\n";
  log << "Loot collected: ";
  for (const auto& l : game.GetLoot ())
    log << l.itemId << "x" << l.quantity << " ";
  log << "\n";

  /* Count alive monsters.  */
  int alive = 0;
  for (const auto& m : game.GetMonsters ())
    if (m.alive) alive++;
  log << "Monsters remaining: " << alive << "/" << game.GetMonsters ().size () << "\n";

  log << "\n=== FINAL MAP ===\n";
  log << RenderMap (game) << "\n";

  /* Print the log.  */
  std::cout << log.str ();

  /* The test "passes" regardless — this is for observation.
     But let's at least assert the game ran.  */
  EXPECT_GT (game.GetTurnCount (), 0);
}

TEST_F (PlaythroughTest, CombatHeavyRun)
{
  /* Use a seed that places monsters near the path.  Level 2 depth
     to get slightly tougher monsters.  */
  auto stats = MakeStats ();
  auto game = DungeonGame::Create ("combat_heavy_v3", 2, stats, 110, 110);

  log << "=== COMBAT HEAVY RUN (Depth 2) ===\n";
  log << "Player at (" << game.GetPlayerX () << ", " << game.GetPlayerY ()
      << ") HP: " << game.GetPlayerHp () << "\n";
  log << "Monsters: " << game.GetMonsters ().size () << "\n";

  for (const auto& m : game.GetMonsters ())
    log << "  " << m.name << " at (" << m.x << "," << m.y
        << ") HP:" << m.hp << " ATK:" << m.attack << "\n";
  log << "\n";

  /* Find furthest gate to maximize travel distance.  */
  const auto& gates = game.GetDungeon ().GetGates ();
  int targetIdx = 0;
  int maxDist = 0;
  for (size_t i = 0; i < gates.size (); i++)
    {
      const int d = std::abs (gates[i].x - game.GetPlayerX ())
                  + std::abs (gates[i].y - game.GetPlayerY ());
      if (d > maxDist)
        {
          maxDist = d;
          targetIdx = static_cast<int> (i);
        }
    }

  const int gateX = gates[targetIdx].x;
  const int gateY = gates[targetIdx].y;
  log << "Target: " << gates[targetIdx].direction
      << " gate at (" << gateX << "," << gateY
      << ") dist=" << maxDist << "\n\n";

  for (int turn = 0; turn < 500 && !game.IsGameOver (); turn++)
    {
      const int px = game.GetPlayerX ();
      const int py = game.GetPlayerY ();
      const int hp = game.GetPlayerHp ();

      /* Use health potion if HP below 40%.  */
      if (hp < game.GetPlayerMaxHp () * 40 / 100)
        {
          Action usePotion;
          usePotion.type = Action::Type::UseItem;
          usePotion.itemId = "health_potion";
          if (game.ProcessAction (usePotion))
            {
              log << "Turn " << turn << ": Used health potion! HP "
                  << hp << " -> " << game.GetPlayerHp () << "\n";
              continue;
            }
        }

      /* Enter gate if we're on it.  */
      if (px == gateX && py == gateY)
        {
          log << "Turn " << turn << ": Entering gate!\n";
          game.ProcessAction ({Action::Type::EnterGate});
          break;
        }

      /* Pick up items we're standing on.  */
      for (const auto& item : game.GetGroundItems ())
        if (item.x == px && item.y == py)
          {
            if (game.ProcessAction ({Action::Type::Pickup}))
              {
                log << "Turn " << turn << ": Picked up " << item.itemId
                    << " x" << item.quantity << "\n";
                goto nextTurn;
              }
          }

      /* Fight adjacent monsters.  */
      for (const auto& m : game.GetMonsters ())
        {
          if (!m.alive)
            continue;
          if (std::abs (m.x - px) <= 1 && std::abs (m.y - py) <= 1)
            {
              auto atkAct = MoveToward (px, py, m.x, m.y);
              game.ProcessAction (atkAct);
              if (!m.alive)
                log << "Turn " << turn << ": Killed " << m.name << "! (+"
                    << m.xpValue << " XP)\n";
              goto nextTurn;
            }
        }

      /* BFS toward gate.  */
      {
        auto [sx, sy] = BfsNextStep (game, px, py, gateX, gateY);
        if (sx != 0 || sy != 0)
          {
            Action mv;
            mv.type = Action::Type::Move;
            mv.dx = sx;
            mv.dy = sy;
            if (!game.ProcessAction (mv))
              game.ProcessAction ({Action::Type::Wait});
          }
        else
          game.ProcessAction ({Action::Type::Wait});
      }

      nextTurn:

      if (turn % 50 == 0 || game.GetPlayerHp () != hp)
        {
          log << "Turn " << turn << ": pos=(" << game.GetPlayerX ()
              << "," << game.GetPlayerY () << ") HP="
              << game.GetPlayerHp () << "/" << game.GetPlayerMaxHp ()
              << " kills=" << game.GetTotalKills ()
              << " gold=" << game.GetTotalGold () << "\n";
        }
    }

  log << "\n=== RESULT ===\n";
  log << "Turns: " << game.GetTurnCount () << "\n";
  log << "Survived: " << (game.HasSurvived () ? "YES" : "NO") << "\n";
  log << "Exit: " << (game.GetExitGate ().empty () ? "(none)" : game.GetExitGate ()) << "\n";
  log << "HP: " << game.GetPlayerHp () << "/" << game.GetPlayerMaxHp () << "\n";
  log << "Kills: " << game.GetTotalKills ()
      << " | XP: " << game.GetTotalXp ()
      << " | Gold: " << game.GetTotalGold () << "\n";
  log << "Loot: ";
  for (const auto& l : game.GetLoot ())
    log << l.itemId << "x" << l.quantity << " ";
  if (game.GetLoot ().empty ()) log << "(none)";
  log << "\n";

  int alive = 0;
  for (const auto& m : game.GetMonsters ())
    if (m.alive) alive++;
  log << "Monsters remaining: " << alive << "/" << game.GetMonsters ().size () << "\n";

  log << "\n" << RenderMap (game) << "\n";

  std::cout << log.str ();
  EXPECT_GT (game.GetTurnCount (), 0);
}

} // anonymous namespace
} // namespace rog
