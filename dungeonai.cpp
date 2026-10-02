#include "dungeonai.hpp"

#include "dungeon.hpp"
#include "items.hpp"

#include <climits>
#include <cmath>
#include <map>
#include <queue>
#include <set>

namespace rog
{

std::pair<int, int>
BfsStepToward (const DungeonGame& game, const int fromX, const int fromY,
               const int toX, const int toY)
{
  using Pos = std::pair<int, int>;
  const auto& dungeon = game.GetDungeon ();

  std::queue<Pos> q;
  std::map<Pos, Pos> parent;
  const Pos start = {fromX, fromY};
  q.push (start);
  parent[start] = {-1, -1};

  static const int dx8[] = {-1, -1, -1, 0, 0, 1, 1, 1};
  static const int dy8[] = {-1, 0, 1, -1, 1, -1, 0, 1};

  while (!q.empty ())
    {
      const auto [cx, cy] = q.front ();
      q.pop ();

      if (cx == toX && cy == toY)
        {
          Pos cur = {toX, toY};
          while (parent[cur] != start)
            cur = parent[cur];
          return {cur.first - fromX, cur.second - fromY};
        }

      for (int i = 0; i < 8; i++)
        {
          const int nx = cx + dx8[i];
          const int ny = cy + dy8[i];
          const Pos next = {nx, ny};
          if (nx < 0 || nx >= Dungeon::WIDTH
              || ny < 0 || ny >= Dungeon::HEIGHT)
            continue;
          if (dungeon.GetTile (nx, ny) == Tile::Wall)
            continue;
          if (parent.count (next))
            continue;
          parent[next] = {cx, cy};
          q.push (next);
        }
    }

  return {0, 0};
}

DungeonGame
PlayToGate (const std::string& seed, const int depth,
            const PlayerStats& stats, const int hp, const int maxHp,
            const DungeonGame::PotionList& potions,
            const std::vector<Gate>& constraints, const std::string& entryDir,
            const std::string& exitDir, const bool collectGold)
{
  auto game = DungeonGame::Create (seed, depth, stats, hp, maxHp, potions,
                                   constraints, entryDir);

  const auto& gates = game.GetDungeon ().GetGates ();
  if (gates.empty ())
    return game;

  /* Target the requested gate, else the nearest one.  */
  int gi = 0;
  int best = INT_MAX;
  for (size_t i = 0; i < gates.size (); i++)
    {
      if (!exitDir.empty ())
        {
          if (gates[i].direction == exitDir)
            gi = static_cast<int> (i);
          continue;
        }
      const int d = std::abs (gates[i].x - game.GetPlayerX ())
                  + std::abs (gates[i].y - game.GetPlayerY ());
      if (d < best)
        {
          best = d;
          gi = static_cast<int> (i);
        }
    }
  const int gateX = gates[gi].x;
  const int gateY = gates[gi].y;

  /* Gold piles found unreachable, so the detour does not retry them.  */
  std::set<std::pair<int, int>> unreachable;

  for (int turn = 0; turn < 1000 && !game.IsGameOver (); turn++)
    {
      const int px = game.GetPlayerX ();
      const int py = game.GetPlayerY ();

      /* Heal if below 30% HP.  */
      if (game.GetPlayerHp () < game.GetPlayerMaxHp () * 30 / 100)
        {
          Action use;
          use.type = Action::Type::UseItem;
          use.itemId = "health_potion";
          if (game.ProcessAction (use))
            continue;
        }

      /* On the gate: exit (a gold run first checks nothing is left).  */
      if (!collectGold && px == gateX && py == gateY)
        {
          game.ProcessAction ({Action::Type::EnterGate});
          break;
        }

      /* Grab anything we're standing on.  */
      bool acted = false;
      for (const auto& it : game.GetGroundItems ())
        if (it.x == px && it.y == py)
          {
            if (game.ProcessAction ({Action::Type::Pickup}))
              acted = true;
            break;
          }
      if (acted)
        continue;

      /* Head for the nearest reachable gold first, if asked to, else
         for the gate (a move into a monster auto-attacks it).  */
      int tx = gateX;
      int ty = gateY;
      if (collectGold)
        {
          int bestGold = INT_MAX;
          for (const auto& it : game.GetGroundItems ())
            {
              if (it.itemId != "gold_coins"
                    || unreachable.count ({it.x, it.y}) > 0)
                continue;
              const int d = std::abs (it.x - px) + std::abs (it.y - py);
              if (d < bestGold)
                {
                  bestGold = d;
                  tx = it.x;
                  ty = it.y;
                }
            }
        }
      if (px == gateX && py == gateY && tx == gateX && ty == gateY)
        {
          game.ProcessAction ({Action::Type::EnterGate});
          break;
        }
      auto [sx, sy] = BfsStepToward (game, px, py, tx, ty);
      if (sx == 0 && sy == 0 && (tx != gateX || ty != gateY))
        {
          unreachable.insert ({tx, ty});
          continue;
        }
      Action mv;
      mv.type = Action::Type::Move;
      mv.dx = sx;
      mv.dy = sy;
      if ((sx != 0 || sy != 0) && game.ProcessAction (mv))
        continue;

      game.ProcessAction ({Action::Type::Wait});
    }

  return game;
}

Json::Value
ActionLogToJson (const std::vector<Action>& actions)
{
  Json::Value arr (Json::arrayValue);
  for (const auto& a : actions)
    {
      Json::Value j (Json::objectValue);
      switch (a.type)
        {
        case Action::Type::Move:
          j["type"] = "move";
          j["dx"] = a.dx;
          j["dy"] = a.dy;
          break;
        case Action::Type::Pickup:
          j["type"] = "pickup";
          break;
        case Action::Type::UseItem:
          j["type"] = "use";
          j["item"] = a.itemId;
          break;
        case Action::Type::EnterGate:
          j["type"] = "gate";
          break;
        case Action::Type::Wait:
          j["type"] = "wait";
          break;
        case Action::Type::Equip:
          j["type"] = "equip";
          j["rowid"] = static_cast<Json::Int64> (a.rowid);
          j["slot"] = a.slot;
          break;
        case Action::Type::Unequip:
          j["type"] = "unequip";
          j["rowid"] = static_cast<Json::Int64> (a.rowid);
          break;
        case Action::Type::Commit:
          j["type"] = "commit";
          j["h"] = a.hex;
          break;
        case Action::Type::Reveal:
          j["type"] = "reveal";
          j["s"] = a.hex;
          break;
        }
      arr.append (j);
    }
  return arr;
}

Json::Value
MergedLogToJson (const std::vector<LoggedAction>& merged)
{
  Json::Value arr (Json::arrayValue);
  for (const auto& la : merged)
    {
      Json::Value j = ActionLogToJson ({la.action})[0];
      j["i"] = la.actor;
      arr.append (j);
    }
  return arr;
}

} // namespace rog
