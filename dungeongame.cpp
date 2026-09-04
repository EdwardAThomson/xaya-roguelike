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

/* Max-HP model shared with the on-chain player table (moveprocessor):
   maxHp = BASE_HP + effectiveConstitution * HP_PER_CON.  item.maxHealth
   is intentionally ignored (matches ComputePlayerStats).  */
constexpr int BASE_HP = 50;
constexpr int HP_PER_CON = 5;

int
RandRange (std::mt19937& rng, const int min, const int max)
{
  std::uniform_int_distribution<int> dist (min, max);
  return dist (rng);
}

/* Adds (sign=+1) or subtracts (sign=-1) an item's six effective-stat
   bonuses.  maxHealth is NOT applied here (see BASE_HP note above).  */
void
ApplyItemBonuses (PlayerStats& stats, const ItemDef& def, const int sign)
{
  stats.equipAttack += sign * def.attackPower;
  stats.equipDefense += sign * def.defense;
  stats.strength += sign * def.strength;
  stats.dexterity += sign * def.dexterity;
  stats.constitution += sign * def.constitution;
  stats.intelligence += sign * def.intelligence;
}

} // anonymous namespace

void
DungeonGame::RecomputeMaxHp (PlayerState& p)
{
  p.maxHp = BASE_HP + p.stats.constitution * HP_PER_CON;
  if (p.maxHp < p.hp)
    p.hp = p.maxHp;
}

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
  /* Bresenham line - check all tiles along the line are non-wall.  */
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

int
DungeonGame::FirstActive () const
{
  for (size_t i = 0; i < players.size (); i++)
    if (IsActive (i))
      return i;
  return -1;
}

int
DungeonGame::NextActiveAfter (const int i) const
{
  for (size_t j = i + 1; j < players.size (); j++)
    if (IsActive (j))
      return j;
  return -1;
}

int
DungeonGame::PlayerAt (const int x, const int y) const
{
  for (size_t i = 0; i < players.size (); i++)
    if (IsActive (i) && players[i].x == x && players[i].y == y)
      return i;
  return -1;
}

bool
DungeonGame::IsWalkable (const int x, const int y, const int self) const
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

  /* Check no other active participant at this position (spec §5; with one
     participant this never triggers, preserving solo behaviour).  */
  const int occ = PlayerAt (x, y);
  if (occ != -1 && occ != self)
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

      /* Never on a participant's spawn tile.  */
      bool onPlayer = false;
      for (const auto& p : players)
        if (x == p.x && y == p.y)
          {
            onPlayer = true;
            break;
          }
      if (onPlayer)
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
DungeonGame::PlayerDied (const int i)
{
  players[i].hp = 0;
  players[i].dead = true;
  if (FirstActive () == -1)
    gameOver = true;
}

void
DungeonGame::PlacePlayer (const int i, const std::string& entryDir)
{
  auto& p = players[i];

  /* Gate entry: spawn one tile inward from that gate.  */
  if (!entryDir.empty ())
    {
      for (const auto& gate : dungeon.GetGates ())
        if (gate.direction == entryDir)
          {
            p.x = gate.x;
            p.y = gate.y;
            if (entryDir == "north") p.y += 1;
            else if (entryDir == "south") p.y -= 1;
            else if (entryDir == "east") p.x -= 1;
            else if (entryDir == "west") p.x += 1;
            return;
          }
    }

  /* Room-centre spawn.  */
  const auto& rooms = dungeon.GetRooms ();
  int cx, cy;
  if (!rooms.empty ())
    {
      cx = rooms[0].centerX ();
      cy = rooms[0].centerY ();
    }
  else
    {
      cx = Dungeon::WIDTH / 2;
      cy = Dungeon::HEIGHT / 2;
    }

  /* Participant 0 takes the centre itself (original solo behaviour).
     Later participants scan outward in a deterministic ring order (spec
     §2a): radius 1, 2, ... with dy-major, dx-minor iteration, first
     in-bounds non-wall tile not occupied by an earlier participant.
     Draws no RNG.  */
  auto taken = [&] (const int x, const int y)
    {
      for (int j = 0; j < i; j++)
        if (players[j].x == x && players[j].y == y)
          return true;
      return false;
    };

  if (!taken (cx, cy))
    {
      p.x = cx;
      p.y = cy;
      return;
    }

  for (int r = 1; r < std::max (Dungeon::WIDTH, Dungeon::HEIGHT); r++)
    for (int dy = -r; dy <= r; dy++)
      for (int dx = -r; dx <= r; dx++)
        {
          if (std::max (std::abs (dx), std::abs (dy)) != r)
            continue;
          const int nx = cx + dx;
          const int ny = cy + dy;
          if (nx < 0 || nx >= Dungeon::WIDTH
              || ny < 0 || ny >= Dungeon::HEIGHT)
            continue;
          if (dungeon.GetTile (nx, ny) == Tile::Wall)
            continue;
          if (taken (nx, ny))
            continue;
          p.x = nx;
          p.y = ny;
          return;
        }

  /* Unreachable in practice; keep the centre as a last resort.  */
  p.x = cx;
  p.y = cy;
}

/* ************************************************************************** */

DungeonGame
DungeonGame::Create (const std::string& seed, const int depth,
                      const PlayerStats& stats, const int hp, const int maxHp,
                      const PotionList& startingPotions,
                      const std::vector<Gate>& constraints,
                      const std::string& entryDir,
                      const EntryInventory& entryInventory)
{
  PlayerSetup setup;
  setup.stats = stats;
  setup.hp = hp;
  setup.maxHp = maxHp;
  setup.potions = startingPotions;
  setup.inventory = entryInventory;
  setup.entryDir = entryDir;
  return CreateMulti (seed, depth, {setup}, constraints);
}

DungeonGame
DungeonGame::CreateMulti (const std::string& seed, const int depth,
                           const std::vector<PlayerSetup>& setups,
                           const std::vector<Gate>& constraints)
{
  DungeonGame game;
  game.depth = depth;
  game.players.assign (setups.size (), PlayerState ());

  for (size_t i = 0; i < setups.size (); i++)
    {
      auto& p = game.players[i];
      const auto& s = setups[i];
      p.stats = s.stats;
      p.hp = s.hp;
      p.maxHp = s.maxHp;

      /* Split the entry inventory into equipped (by slot) and bag.  The
         stats passed in are ALREADY effective (base + entry-equipped), so
         we do NOT re-apply equipped bonuses here; equip/unequip actions
         mutate by delta.  */
      for (const auto& item : s.inventory)
        {
          if (item.slot == "bag")
            p.bag.push_back ({item.rowid, item.itemId});
          else
            p.equipped[item.slot] = {item.rowid, item.itemId};
        }
    }

  game.turnCount = 0;
  game.gameOver = false;
  game.curTurn = 0;

  /* Seed the RNG from the dungeon seed (FNV-1a - cross-language).  */
  game.rng = std::mt19937 (
      HashSeed (seed + ":game:" + std::to_string (depth)));

  /* Generate the dungeon.  When the segment was discovered with a gate
     aligned to its neighbour, regenerate with that same constraint so the
     layout is byte-identical to discovery (and to the frontend).  */
  game.dungeon = constraints.empty ()
      ? Dungeon::Generate (seed, depth)
      : Dungeon::Generate (seed, depth, constraints);

  /* Place the participants in canonical order (draws no RNG).  */
  for (size_t i = 0; i < setups.size (); i++)
    game.PlacePlayer (i, setups[i].entryDir);

  /* Spawn monsters (away from players).  */
  game.monsters = SpawnMonsters (game.dungeon, depth, game.rng);

  /* Remove any monster that spawned on or near any participant.  */
  game.monsters.erase (
    std::remove_if (game.monsters.begin (), game.monsters.end (),
      [&game] (const Monster& m)
        {
          for (const auto& p : game.players)
            if (ManhattanDist (m.x, m.y, p.x, p.y) < 5)
              return true;
          return false;
        }),
    game.monsters.end ());

  /* Spawn ground items.  */
  game.SpawnGroundItems ();

  /* Add each participant's starting potions to their session loot.  */
  for (size_t i = 0; i < setups.size (); i++)
    for (const auto& [potionId, qty] : setups[i].potions)
      if (qty > 0)
        game.players[i].loot.push_back ({potionId, qty});

  return game;
}

DungeonGame
DungeonGame::Replay (const std::string& seed, const int depth,
                      const PlayerStats& stats, const int hp, const int maxHp,
                      const PotionList& startingPotions,
                      const std::vector<Action>& actions,
                      const std::vector<Gate>& constraints,
                      const std::string& entryDir,
                      const EntryInventory& entryInventory)
{
  auto game = Create (seed, depth, stats, hp, maxHp, startingPotions,
                      constraints, entryDir, entryInventory);

  for (const auto& action : actions)
    {
      if (!game.ProcessAction (action))
        break;  /* Invalid action - stop replay here.  */
    }

  return game;
}

DungeonGame
DungeonGame::ReplayMulti (const std::string& seed, const int depth,
                           const std::vector<PlayerSetup>& setups,
                           const std::vector<LoggedAction>& actions,
                           const std::vector<Gate>& constraints)
{
  auto game = CreateMulti (seed, depth, setups, constraints);

  for (const auto& la : actions)
    {
      if (!game.ProcessAction (la.actor, la.action))
        break;  /* Invalid action or wrong turn - stop replay here.  */
    }

  return game;
}

/* ************************************************************************** */

bool
DungeonGame::ProcessAction (const int actor, const Action& action)
{
  if (gameOver)
    return false;

  /* Round structure (spec §2): only the expected participant may act.
     With one participant this is always index 0.  */
  if (actor < 0 || actor >= static_cast<int> (players.size ()))
    return false;
  if (actor != curTurn || !IsActive (actor))
    return false;

  auto& p = players[actor];
  bool validAction = false;

  switch (action.type)
    {
    case Action::Type::Move:
      {
        if (action.dx < -1 || action.dx > 1
            || action.dy < -1 || action.dy > 1
            || (action.dx == 0 && action.dy == 0))
          return false;

        const int nx = p.x + action.dx;
        const int ny = p.y + action.dy;

        /* Moving into a monster = attack.  */
        Monster* target = MonsterAt (nx, ny);
        if (target != nullptr)
          {
            auto result = PlayerAttackMonster (p.stats, target->defense, rng);
            if (result.hit)
              {
                /* Contribution is capped at the target's remaining HP so
                   overkill does not inflate the pro-rata split (spec §5).  */
                p.damageDealt += std::min (result.damage, target->hp);
                target->hp -= result.damage;
                if (target->hp <= 0)
                  {
                    target->alive = false;
                    /* XP per kill scales with depth so pushing deeper levels
                       faster: floor(xpValue * (1 + (depth-1) * 0.15)).  The
                       award goes to the killer's own counter (what solo
                       claims verify against) AND the run pool (what the
                       multiplayer pro-rata split draws from).  */
                    const int xpAward = static_cast<int> (std::floor (
                        target->xpValue * (1.0 + (depth - 1) * 0.15)));
                    p.totalXp += xpAward;
                    xpPool += xpAward;
                    p.totalKills++;

                    /* Monster drops (35% chance). */
                    if (RandRange (rng, 1, 100) <= 35)
                      {
                        const int dropRoll = RandRange (rng, 1, 100);
                        if (dropRoll <= 50)
                          {
                            /* Gold.  Multiplayer: to the kill-gold pool for
                               the pro-rata split.  Solo: to the floor,
                               byte-identical to the original behaviour.  */
                            const int amt = RandRange (rng, 1, 5 + depth * 3);
                            if (players.size () > 1)
                              killGoldPool += amt;
                            else
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
        else if (IsWalkable (nx, ny, actor))
          {
            p.x = nx;
            p.y = ny;
            validAction = true;
          }
        else
          return false;  /* Can't move there.  */
      }
      break;

    case Action::Type::Pickup:
      {
        GroundItem* item = ItemAt (p.x, p.y);
        if (item == nullptr)
          return false;

        /* Gold goes directly to total.  */
        if (item->itemId == "gold_coins")
          {
            p.totalGold += item->quantity;
          }
        else
          {
            /* Add to loot.  */
            bool found = false;
            for (auto& l : p.loot)
              if (l.itemId == item->itemId)
                {
                  l.quantity += item->quantity;
                  found = true;
                  break;
                }
            if (!found)
              p.loot.push_back ({item->itemId, item->quantity});
          }

        /* Remove from ground.  */
        groundItems.erase (
          std::remove_if (groundItems.begin (), groundItems.end (),
            [&] (const GroundItem& gi)
              { return gi.x == p.x && gi.y == p.y
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

        /* Check if the participant has this item in session loot.  */
        bool used = false;
        for (auto& l : p.loot)
          if (l.itemId == action.itemId && l.quantity > 0)
            {
              l.quantity--;
              p.hp = std::min (p.hp + def->healAmount, p.maxHp);
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
        /* Check the participant is on a gate tile.  */
        if (dungeon.GetTile (p.x, p.y) != Tile::Gate)
          return false;

        /* Find which gate this is.  */
        for (const auto& gate : dungeon.GetGates ())
          if (gate.x == p.x && gate.y == p.y)
            {
              p.exitGate = gate.direction;
              break;
            }

        p.exited = true;
        if (FirstActive () == -1)
          gameOver = true;
        validAction = true;
      }
      break;

    case Action::Type::Equip:
      {
        /* Must be a banked bag item (this-run pickups live in `loot`).  */
        auto it = std::find_if (p.bag.begin (), p.bag.end (),
            [&] (const BagItem& b) { return b.rowid == action.rowid; });
        if (it == p.bag.end ())
          return false;

        const std::string newItemId = it->itemId;
        const ItemDef* def = LookupItem (newItemId);
        if (def == nullptr || def->slot.empty () || def->slot != action.slot)
          return false;

        /* Displace whatever currently occupies the slot back to the bag,
           subtracting its bonuses first.  */
        auto occ = p.equipped.find (action.slot);
        if (occ != p.equipped.end ())
          {
            const ItemDef* oldDef = LookupItem (occ->second.itemId);
            if (oldDef != nullptr)
              ApplyItemBonuses (p.stats, *oldDef, -1);
            p.bag.push_back ({occ->second.rowid, occ->second.itemId});
            p.equipped.erase (occ);
          }

        /* Remove the new item from the bag and equip it.  */
        p.bag.erase (
          std::remove_if (p.bag.begin (), p.bag.end (),
            [&] (const BagItem& b) { return b.rowid == action.rowid; }),
          p.bag.end ());
        ApplyItemBonuses (p.stats, *def, +1);
        p.equipped[action.slot] = {action.rowid, newItemId};

        RecomputeMaxHp (p);
        validAction = true;
      }
      break;

    case Action::Type::Unequip:
      {
        /* Find which slot holds this rowid.  */
        std::string foundSlot;
        for (const auto& [slot, e] : p.equipped)
          if (e.rowid == action.rowid)
            {
              foundSlot = slot;
              break;
            }
        if (foundSlot.empty ())
          return false;

        auto occ = p.equipped.find (foundSlot);
        const ItemDef* oldDef = LookupItem (occ->second.itemId);
        if (oldDef != nullptr)
          ApplyItemBonuses (p.stats, *oldDef, -1);
        p.bag.push_back ({occ->second.rowid, occ->second.itemId});
        p.equipped.erase (occ);

        RecomputeMaxHp (p);
        validAction = true;
      }
      break;

    case Action::Type::Wait:
      validAction = true;
      break;
    }

  if (!validAction)
    return false;

  actionLog.push_back (action);
  mergedLog.push_back ({actor, action});
  turnCount++;

  /* Round advance (spec §2): after the last active participant of the
     round, monsters act once; otherwise pass the turn along.  With one
     participant this reduces to "monsters act after the player".  */
  const int next = NextActiveAfter (actor);
  if (next == -1)
    {
      if (!gameOver)
        ProcessMonsterTurns ();
      const int first = FirstActive ();
      curTurn = first == -1 ? 0 : first;
    }
  else
    curTurn = next;

  return true;
}

std::vector<LoadoutEntry>
DungeonGame::GetFinalInventory (const int i) const
{
  std::vector<LoadoutEntry> result;
  for (const auto& [slot, e] : players[i].equipped)
    result.push_back ({e.rowid, slot});
  for (const auto& b : players[i].bag)
    result.push_back ({b.rowid, "bag"});
  return result;
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
  /* Check awareness: any active participant in range with line of sight
     (spec §4).  Iterate in canonical order with an early out.  */
  if (!m.awareOfPlayer)
    for (size_t i = 0; i < players.size (); i++)
      {
        if (!IsActive (i))
          continue;
        const auto& p = players[i];
        if (ManhattanDist (m.x, m.y, p.x, p.y) <= m.detectionRange
            && HasLineOfSight (m.x, m.y, p.x, p.y))
          {
            m.awareOfPlayer = true;
            break;
          }
      }

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
              && PlayerAt (nx, ny) == -1
              && MonsterAt (nx, ny) == nullptr)
            {
              m.x = nx;
              m.y = ny;
            }
        }
      return;
    }

  /* Monster is aware.  Target the nearest active participant by Manhattan
     distance, ties to the lower index (spec §4); recomputed every act.  */
  int target = -1;
  int targetDist = 0;
  for (size_t i = 0; i < players.size (); i++)
    {
      if (!IsActive (i))
        continue;
      const int d = ManhattanDist (m.x, m.y, players[i].x, players[i].y);
      if (target == -1 || d < targetDist)
        {
          target = i;
          targetDist = d;
        }
    }
  if (target == -1)
    return;  /* No active participants (defensive; caller stops on gameOver).  */

  auto& tp = players[target];

  /* If adjacent (including diagonal), attack.  */
  if (std::abs (m.x - tp.x) <= 1 && std::abs (m.y - tp.y) <= 1)
    {
      auto result = MonsterAttackPlayer (m.attack, m.critChance, tp.stats, rng);
      if (result.hit)
        {
          tp.hp -= result.damage;
          if (tp.hp <= 0)
            PlayerDied (target);
        }
      return;
    }

  /* Move toward the target (simple: pick the adjacent tile that minimizes
     Manhattan distance).  */
  int bestDist = targetDist;
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
        if (PlayerAt (nx, ny) != -1)
          continue;  /* Don't move onto a player - attack instead.  */
        if (MonsterAt (nx, ny) != nullptr)
          continue;

        const int d = ManhattanDist (nx, ny, tp.x, tp.y);
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
  auto& p = players[0];
  p.x = px;
  p.y = py;
  p.hp = hp;
  p.maxHp = maxHp;
  turnCount = turns;
  p.totalXp = xp;
  p.totalGold = gold;
  p.totalKills = kills;
  gameOver = over;
  p.exited = surv;
  p.dead = over && !surv;
  p.exitGate = gate;
}

} // namespace rog
