#include "dungeongame.hpp"
#include "hash.hpp"
#include "items.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace rog
{

namespace
{

int
RandRange (std::mt19937& rng, const int min, const int max)
{
  std::uniform_int_distribution<int> dist (min, max);
  return dist (rng);
}

} // anonymous namespace

/* ************************************************************************** */

int
DungeonGame::ManhattanDist (const int x1, const int y1,
                             const int x2, const int y2)
{
  return std::abs (x1 - x2) + std::abs (y1 - y2);
}

bool
DungeonGame::HasLineOfSight (const int x1, const int y1,
                              const int x2, const int y2) const
{
  /* Bresenham line — check all tiles along the line are non-wall.  */
  int dx = std::abs (x2 - x1);
  int dy = -std::abs (y2 - y1);
  int sx = x1 < x2 ? 1 : -1;
  int sy = y1 < y2 ? 1 : -1;
  int err = dx + dy;

  int cx = x1, cy = y1;
  while (cx != x2 || cy != y2)
    {
      if (dungeon.GetTile (cx, cy) == Tile::Wall
          && !(cx == x1 && cy == y1))
        return false;

      int e2 = 2 * err;
      if (e2 >= dy)
        {
          err += dy;
          cx += sx;
        }
      if (e2 <= dx)
        {
          err += dx;
          cy += sy;
        }
    }
  return true;
}

bool
DungeonGame::IsWalkable (const int x, const int y) const
{
  if (x < 0 || x >= Dungeon::WIDTH || y < 0 || y >= Dungeon::HEIGHT)
    return false;

  const Tile t = dungeon.GetTile (x, y);
  if (t == Tile::Wall)
    return false;

  /* Check no living monster at this position.  */
  for (const auto& m : monsters)
    if (m.alive && m.x == x && m.y == y)
      return false;

  return true;
}

Monster*
DungeonGame::MonsterAt (const int x, const int y)
{
  for (auto& m : monsters)
    if (m.alive && m.x == x && m.y == y)
      return &m;
  return nullptr;
}

GroundItem*
DungeonGame::ItemAt (const int x, const int y)
{
  for (auto& item : groundItems)
    if (item.x == x && item.y == y)
      return &item;
  return nullptr;
}

void
DungeonGame::SpawnGroundItems ()
{
  const int count = RandRange (rng, 6, 12);

  /* Get items appropriate for this depth.  */
  auto spawnable = GetSpawnableItems (depth);

  /* Always include gold and health potions.  */
  const auto* goldDef = LookupItem ("gold_coins");
  const auto* potionDef = LookupItem ("health_potion");

  for (int i = 0; i < count; i++)
    {
      auto [x, y] = dungeon.GetRandomFloorPosition (rng);
      if (x < 0)
        continue;

      if (x == playerX && y == playerY)
        continue;

      GroundItem gi;
      gi.x = x;
      gi.y = y;

      /* 30% gold, 25% health potion, 45% random equipment/item.  */
      const int roll = RandRange (rng, 1, 100);
      if (roll <= 30 && goldDef != nullptr)
        {
          gi.itemId = "gold_coins";
          gi.quantity = RandRange (rng, 1 + depth, 5 + depth * 3);
        }
      else if (roll <= 55 && potionDef != nullptr)
        {
          gi.itemId = "health_potion";
          gi.quantity = 1;
        }
      else if (!spawnable.empty ())
        {
          std::uniform_int_distribution<size_t> dist (0, spawnable.size () - 1);
          gi.itemId = spawnable[dist (rng)]->id;
          gi.quantity = 1;
        }
      else
        continue;

      groundItems.push_back (gi);
    }
}

void
DungeonGame::PlayerDied ()
{
  playerHp = 0;
  gameOver = true;
  survived = false;
}

/* ************************************************************************** */

DungeonGame
DungeonGame::Create (const std::string& seed, const int depth,
                      const PlayerStats& stats, const int hp, const int maxHp,
                      const PotionList& startingPotions)
{
  DungeonGame game;
  game.depth = depth;
  game.stats = stats;
  game.playerHp = hp;
  game.playerMaxHp = maxHp;
  game.turnCount = 0;
  game.totalXp = 0;
  game.totalGold = 0;
  game.totalKills = 0;
  game.gameOver = false;
  game.survived = false;

  /* Seed the RNG from the dungeon seed (FNV-1a — cross-language).  */
  game.rng = std::mt19937 (
      HashSeed (seed + ":game:" + std::to_string (depth)));

  /* Generate the dungeon.  */
  game.dungeon = Dungeon::Generate (seed, depth);

  /* Place player at the center of the first room.  */
  const auto& rooms = game.dungeon.GetRooms ();
  if (!rooms.empty ())
    {
      game.playerX = rooms[0].centerX ();
      game.playerY = rooms[0].centerY ();
    }
  else
    {
      game.playerX = Dungeon::WIDTH / 2;
      game.playerY = Dungeon::HEIGHT / 2;
    }

  /* Spawn monsters (away from player).  */
  game.monsters = SpawnMonsters (game.dungeon, depth, game.rng);

  /* Remove any monster that spawned on or adjacent to the player.  */
  game.monsters.erase (
    std::remove_if (game.monsters.begin (), game.monsters.end (),
      [&game] (const Monster& m)
        {
          return ManhattanDist (m.x, m.y, game.playerX, game.playerY) < 5;
        }),
    game.monsters.end ());

  /* Spawn ground items.  */
  game.SpawnGroundItems ();

  /* Add starting potions from player's inventory to the session loot.  */
  for (const auto& [potionId, qty] : startingPotions)
    if (qty > 0)
      game.loot.push_back ({potionId, qty});

  return game;
}

/* ************************************************************************** */

bool
DungeonGame::ProcessAction (const Action& action)
{
  if (gameOver)
    return false;

  bool validAction = false;

  switch (action.type)
    {
    case Action::Type::Move:
      {
        if (action.dx < -1 || action.dx > 1
            || action.dy < -1 || action.dy > 1
            || (action.dx == 0 && action.dy == 0))
          return false;

        const int nx = playerX + action.dx;
        const int ny = playerY + action.dy;

        /* Moving into a monster = attack.  */
        Monster* target = MonsterAt (nx, ny);
        if (target != nullptr)
          {
            auto result = PlayerAttackMonster (stats, target->defense, rng);
            if (result.hit)
              {
                target->hp -= result.damage;
                if (target->hp <= 0)
                  {
                    target->alive = false;
                    totalXp += target->xpValue;
                    totalKills++;

                    /* Monster drops (35% chance). */
                    if (RandRange (rng, 1, 100) <= 35)
                      {
                        const int dropRoll = RandRange (rng, 1, 100);
                        if (dropRoll <= 50)
                          {
                            /* Gold. */
                            const int amt = RandRange (rng, 1, 5 + depth * 3);
                            groundItems.push_back (
                                {target->x, target->y, "gold_coins", amt});
                          }
                        else if (dropRoll <= 75)
                          {
                            groundItems.push_back (
                                {target->x, target->y, "health_potion", 1});
                          }
                        else
                          {
                            /* Random equipment. */
                            auto spawnable = GetSpawnableItems (depth);
                            if (!spawnable.empty ())
                              {
                                std::uniform_int_distribution<size_t> dist (
                                    0, spawnable.size () - 1);
                                groundItems.push_back (
                                    {target->x, target->y,
                                     spawnable[dist (rng)]->id, 1});
                              }
                          }
                      }
                  }
              }
            validAction = true;
          }
        else if (IsWalkable (nx, ny))
          {
            playerX = nx;
            playerY = ny;
            validAction = true;
          }
        else
          return false;  /* Can't move there.  */
      }
      break;

    case Action::Type::Pickup:
      {
        GroundItem* item = ItemAt (playerX, playerY);
        if (item == nullptr)
          return false;

        /* Gold goes directly to total.  */
        if (item->itemId == "gold_coins")
          {
            totalGold += item->quantity;
          }
        else
          {
            /* Add to loot.  */
            bool found = false;
            for (auto& l : loot)
              if (l.itemId == item->itemId)
                {
                  l.quantity += item->quantity;
                  found = true;
                  break;
                }
            if (!found)
              loot.push_back ({item->itemId, item->quantity});
          }

        /* Remove from ground.  */
        groundItems.erase (
          std::remove_if (groundItems.begin (), groundItems.end (),
            [&] (const GroundItem& gi)
              { return gi.x == playerX && gi.y == playerY
                       && gi.itemId == item->itemId; }),
          groundItems.end ());

        validAction = true;
      }
      break;

    case Action::Type::UseItem:
      {
        const ItemDef* def = LookupItem (action.itemId);
        if (def == nullptr || !def->consumable || def->healAmount <= 0)
          return false;

        /* Check if player has this item in session loot.  */
        bool used = false;
        for (auto& l : loot)
          if (l.itemId == action.itemId && l.quantity > 0)
            {
              l.quantity--;
              playerHp = std::min (playerHp + def->healAmount, playerMaxHp);
              used = true;
              break;
            }
        if (!used)
          return false;

        validAction = true;
      }
      break;

    case Action::Type::EnterGate:
      {
        /* Check player is on a gate tile.  */
        if (dungeon.GetTile (playerX, playerY) != Tile::Gate)
          return false;

        /* Find which gate this is.  */
        for (const auto& gate : dungeon.GetGates ())
          if (gate.x == playerX && gate.y == playerY)
            {
              exitGate = gate.direction;
              break;
            }

        gameOver = true;
        survived = true;
        validAction = true;
      }
      break;

    case Action::Type::Wait:
      validAction = true;
      break;
    }

  if (!validAction)
    return false;

  turnCount++;

  /* Monsters act after the player.  */
  if (!gameOver)
    ProcessMonsterTurns ();

  return true;
}

/* ************************************************************************** */

void
DungeonGame::ProcessMonsterTurns ()
{
  for (auto& m : monsters)
    {
      if (!m.alive)
        continue;
      MonsterAct (m);
      if (gameOver)
        return;
    }
}

void
DungeonGame::MonsterAct (Monster& m)
{
  const int dist = ManhattanDist (m.x, m.y, playerX, playerY);

  /* Check awareness.  */
  if (!m.awareOfPlayer && dist <= m.detectionRange
      && HasLineOfSight (m.x, m.y, playerX, playerY))
    m.awareOfPlayer = true;

  if (!m.awareOfPlayer)
    {
      /* Random movement (25% chance to move).  */
      if (RandRange (rng, 1, 4) == 1)
        {
          const int dx = RandRange (rng, -1, 1);
          const int dy = RandRange (rng, -1, 1);
          const int nx = m.x + dx;
          const int ny = m.y + dy;

          if (nx >= 0 && nx < Dungeon::WIDTH
              && ny >= 0 && ny < Dungeon::HEIGHT
              && dungeon.GetTile (nx, ny) != Tile::Wall
              && !(nx == playerX && ny == playerY)
              && MonsterAt (nx, ny) == nullptr)
            {
              m.x = nx;
              m.y = ny;
            }
        }
      return;
    }

  /* Monster is aware of player.  */

  /* If adjacent (including diagonal), attack.  */
  if (std::abs (m.x - playerX) <= 1 && std::abs (m.y - playerY) <= 1)
    {
      auto result = MonsterAttackPlayer (m.attack, m.critChance, stats, rng);
      if (result.hit)
        {
          playerHp -= result.damage;
          if (playerHp <= 0)
            PlayerDied ();
        }
      return;
    }

  /* Move toward player (simple: pick the adjacent tile that minimizes
     Manhattan distance).  */
  int bestDist = dist;
  int bestX = m.x, bestY = m.y;

  for (int dx = -1; dx <= 1; dx++)
    for (int dy = -1; dy <= 1; dy++)
      {
        if (dx == 0 && dy == 0)
          continue;

        const int nx = m.x + dx;
        const int ny = m.y + dy;

        if (nx < 0 || nx >= Dungeon::WIDTH
            || ny < 0 || ny >= Dungeon::HEIGHT)
          continue;
        if (dungeon.GetTile (nx, ny) == Tile::Wall)
          continue;
        if (nx == playerX && ny == playerY)
          continue;  /* Don't move onto player — attack instead (handled above).  */
        if (MonsterAt (nx, ny) != nullptr)
          continue;

        const int d = ManhattanDist (nx, ny, playerX, playerY);
        if (d < bestDist)
          {
            bestDist = d;
            bestX = nx;
            bestY = ny;
          }
      }

  m.x = bestX;
  m.y = bestY;
}

/* ************************************************************************** */

std::string
DungeonGame::SerializeRng () const
{
  std::ostringstream oss;
  oss << rng;
  return oss.str ();
}

void
DungeonGame::RestoreRng (const std::string& data)
{
  std::istringstream iss (data);
  iss >> rng;
}

void
DungeonGame::SetState (const int px, const int py, const int hp,
                        const int maxHp, const int turns,
                        const int xp, const int gold, const int kills,
                        const bool over, const bool surv,
                        const std::string& gate)
{
  playerX = px;
  playerY = py;
  playerHp = hp;
  playerMaxHp = maxHp;
  turnCount = turns;
  totalXp = xp;
  totalGold = gold;
  totalKills = kills;
  gameOver = over;
  survived = surv;
  exitGate = gate;
}

} // namespace rog
