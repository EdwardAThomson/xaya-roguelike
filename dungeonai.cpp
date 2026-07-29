#include "dungeonai.hpp"

#include "dungeon.hpp"
#include "items.hpp"

#include <climits>
#include <cmath>
#include <map>
#include <queue>

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
            const std::vector<Gate>& constraints, const std::string& entryDir)
{
  auto game = DungeonGame::Create (seed, depth, stats, hp, maxHp, potions,
                                   constraints, entryDir);

  const auto& gates = game.GetDungeon ().GetGates ();
  if (gates.empty ())
    return game;

  /* Target the nearest gate.  */
  int gi = 0;
  int best = INT_MAX;
  for (size_t i = 0; i < gates.size (); i++)
    {
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

      /* On the gate: exit.  */
      if (px == gateX && py == gateY)
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

      /* Step toward the gate (a move into a monster auto-attacks it).  */
      const auto [sx, sy] = BfsStepToward (game, px, py, gateX, gateY);
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
        }
      arr.append (j);
    }
  return arr;
}

} // namespace rog
