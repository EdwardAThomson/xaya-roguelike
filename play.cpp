/**
 * Interactive dungeon play binary.  Communicates via JSON on stdin/stdout.
 *
 * Usage: roguelike-play seed depth hp maxhp [level str dex con int eqAtk eqDef potions]
 *
 * Each turn:
 *   1. Prints game state as JSON to stdout (one line)
 *   2. Reads an action as JSON from stdin (one line)
 *   3. Processes the action and repeats
 *
 * Action format:
 *   {"action": "move", "dx": 1, "dy": 0}
 *   {"action": "wait"}
 *   {"action": "pickup"}
 *   {"action": "use", "item": "health_potion"}
 *   {"action": "gate"}
 *
 * Solve mode (non-interactive winning-proof generator for tests/tooling):
 *   roguelike-play --solve seed depth hp maxhp [level str dex con int eqAtk eqDef potions]
 *
 * Drives the deterministic dungeon AI (PlayToGate) to a gate and prints a
 * single JSON line with the claimed results and the full action proof:
 *   {"survived":true,"xp":..,"gold":..,"kills":..,"hp_remaining":..,
 *    "actions":[{"type":"move","dx":1,"dy":0}, ...]}
 * Exit code 0 if a gate was reached (survived), 1 otherwise.  The action
 * log replays byte-for-byte through the GSP (DungeonGame::Replay).
 */

#include "dungeongame.hpp"
#include "dungeonai.hpp"
#include "dungeon.hpp"
#include "combat.hpp"
#include "items.hpp"

#include <json/json.h>

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

Json::Value
GameStateToJson (const rog::DungeonGame& game)
{
  Json::Value state (Json::objectValue);
  state["turn"] = game.GetTurnCount ();
  state["player_x"] = game.GetPlayerX ();
  state["player_y"] = game.GetPlayerY ();
  state["hp"] = game.GetPlayerHp ();
  state["max_hp"] = game.GetPlayerMaxHp ();
  state["game_over"] = game.IsGameOver ();
  state["survived"] = game.HasSurvived ();
  state["exit_gate"] = game.GetExitGate ();
  state["kills"] = game.GetTotalKills ();
  state["xp"] = game.GetTotalXp ();
  state["gold"] = game.GetTotalGold ();
  state["depth"] = game.GetDepth ();

  /* Loot.  */
  Json::Value lootJson (Json::arrayValue);
  for (const auto& l : game.GetLoot ())
    {
      Json::Value lj (Json::objectValue);
      lj["item"] = l.itemId;
      lj["qty"] = l.quantity;
      lootJson.append (lj);
    }
  state["loot"] = lootJson;

  /* Monsters (alive only).  */
  Json::Value monstersJson (Json::arrayValue);
  for (const auto& m : game.GetMonsters ())
    if (m.alive)
      {
        Json::Value mj (Json::objectValue);
        mj["name"] = m.name;
        mj["x"] = m.x;
        mj["y"] = m.y;
        mj["hp"] = m.hp;
        mj["max_hp"] = m.maxHp;
        mj["attack"] = m.attack;
        mj["symbol"] = m.symbol;
        monstersJson.append (mj);
      }
  state["monsters"] = monstersJson;

  /* Ground items.  */
  Json::Value itemsJson (Json::arrayValue);
  for (const auto& gi : game.GetGroundItems ())
    {
      Json::Value ij (Json::objectValue);
      ij["item"] = gi.itemId;
      ij["x"] = gi.x;
      ij["y"] = gi.y;
      ij["qty"] = gi.quantity;
      itemsJson.append (ij);
    }
  state["ground_items"] = itemsJson;

  /* Gates.  */
  Json::Value gatesJson (Json::arrayValue);
  for (const auto& g : game.GetDungeon ().GetGates ())
    {
      Json::Value gj (Json::objectValue);
      gj["dir"] = g.direction;
      gj["x"] = g.x;
      gj["y"] = g.y;
      gatesJson.append (gj);
    }
  state["gates"] = gatesJson;

  /* ASCII map (visible area around player).  */
  const int px = game.GetPlayerX ();
  const int py = game.GetPlayerY ();
  const int radius = 12;
  int minX = std::max (0, px - radius);
  int maxX = std::min (rog::Dungeon::WIDTH - 1, px + radius);
  int minY = std::max (0, py - radius);
  int maxY = std::min (rog::Dungeon::HEIGHT - 1, py + radius);

  /* Always output full map — BFS pathfinding needs the whole grid.  */
  minX = 0;
  maxX = rog::Dungeon::WIDTH - 1;
  minY = 0;
  maxY = rog::Dungeon::HEIGHT - 1;

  std::string map;
  for (int y = minY; y <= maxY; y++)
    {
      for (int x = minX; x <= maxX; x++)
        {
          if (x == px && y == py)
            {
              map += '@';
              continue;
            }

          bool drawn = false;
          for (const auto& m : game.GetMonsters ())
            if (m.alive && m.x == x && m.y == y)
              {
                map += m.symbol[0];
                drawn = true;
                break;
              }
          if (drawn)
            continue;

          for (const auto& gi : game.GetGroundItems ())
            if (gi.x == x && gi.y == y)
              {
                map += '!';
                drawn = true;
                break;
              }
          if (drawn)
            continue;

          const auto t = game.GetDungeon ().GetTile (x, y);
          switch (t)
            {
              case rog::Tile::Wall: map += '#'; break;
              case rog::Tile::Floor: map += '.'; break;
              case rog::Tile::Gate: map += '+'; break;
            }
        }
      map += '\n';
    }
  state["map"] = map;
  state["map_origin_x"] = minX;
  state["map_origin_y"] = minY;

  return state;
}

rog::Action
ParseAction (const Json::Value& input)
{
  rog::Action action;
  const std::string type = input["action"].asString ();

  if (type == "move")
    {
      action.type = rog::Action::Type::Move;
      action.dx = input["dx"].asInt ();
      action.dy = input["dy"].asInt ();
    }
  else if (type == "wait")
    action.type = rog::Action::Type::Wait;
  else if (type == "pickup")
    action.type = rog::Action::Type::Pickup;
  else if (type == "use")
    {
      action.type = rog::Action::Type::UseItem;
      action.itemId = input["item"].asString ();
    }
  else if (type == "gate")
    action.type = rog::Action::Type::EnterGate;
  else
    {
      /* Default to wait for unknown actions.  */
      action.type = rog::Action::Type::Wait;
    }

  return action;
}

} // anonymous namespace

namespace
{

/**
 * Parses the positional game arguments (seed depth hp maxhp [stats...])
 * starting at argv[base].  Shared by interactive and solve modes.
 */
void
ParseGameArgs (int argc, char** argv, int base, std::string& seed,
               int& depth, int& hp, int& maxHp, rog::PlayerStats& stats,
               rog::DungeonGame::PotionList& potions)
{
  seed  = argc > base + 0 ? argv[base + 0] : "default_seed";
  depth = argc > base + 1 ? std::atoi (argv[base + 1]) : 1;
  hp    = argc > base + 2 ? std::atoi (argv[base + 2]) : 100;
  maxHp = argc > base + 3 ? std::atoi (argv[base + 3]) : 100;

  stats.level        = argc > base + 4  ? std::atoi (argv[base + 4])  : 1;
  stats.strength     = argc > base + 5  ? std::atoi (argv[base + 5])  : 10;
  stats.dexterity    = argc > base + 6  ? std::atoi (argv[base + 6])  : 10;
  stats.constitution = argc > base + 7  ? std::atoi (argv[base + 7])  : 10;
  stats.intelligence = argc > base + 8  ? std::atoi (argv[base + 8])  : 10;
  stats.equipAttack  = argc > base + 9  ? std::atoi (argv[base + 9])  : 5;
  stats.equipDefense = argc > base + 10 ? std::atoi (argv[base + 10]) : 2;

  const int numPotions = argc > base + 11 ? std::atoi (argv[base + 11]) : 3;
  if (numPotions > 0)
    potions.push_back ({"health_potion", numPotions});
}

/**
 * Non-interactive solve mode: drive the AI to a gate and print the
 * winning proof (results + action log) as one JSON line.
 *
 * Two input forms after `--solve`:
 *   1. Positional: seed depth hp maxhp [level str dex con int eqAtk eqDef pot]
 *      (unconstrained, centre spawn — for quick manual use).
 *   2. JSON spec (a single arg starting with '{'), which also carries the
 *      segment's alignment constraints and the entry direction so the
 *      generated layout/spawn match a constrained or gate-walked run:
 *        {"seed":"..","depth":1,"hp":100,"max_hp":100,
 *         "stats":{"level":1,"strength":10,"dexterity":10,
 *                  "constitution":10,"intelligence":10,
 *                  "equip_attack":5,"equip_defense":2},
 *         "potions":3,"entry_direction":"",
 *         "constraints":[{"x":0,"y":6,"direction":"west"}]}
 */
int
RunSolve (int argc, char** argv)
{
  std::string seed;
  int depth = 1, hp = 100, maxHp = 100;
  rog::PlayerStats stats;
  rog::DungeonGame::PotionList potions;
  std::vector<rog::Gate> constraints;
  std::string entryDir;

  if (argc > 2 && argv[2][0] == '{')
    {
      Json::Value spec;
      Json::CharReaderBuilder reader;
      std::istringstream iss (argv[2]);
      std::string errs;
      if (!Json::parseFromStream (reader, iss, &spec, &errs))
        {
          std::cerr << "Invalid --solve JSON: " << errs << std::endl;
          return 2;
        }

      seed  = spec.get ("seed", "default_seed").asString ();
      depth = spec.get ("depth", 1).asInt ();
      hp    = spec.get ("hp", 100).asInt ();
      maxHp = spec.get ("max_hp", 100).asInt ();
      entryDir = spec.get ("entry_direction", "").asString ();

      const Json::Value& s = spec["stats"];
      stats.level        = s.get ("level", 1).asInt ();
      stats.strength     = s.get ("strength", 10).asInt ();
      stats.dexterity    = s.get ("dexterity", 10).asInt ();
      stats.constitution = s.get ("constitution", 10).asInt ();
      stats.intelligence = s.get ("intelligence", 10).asInt ();
      stats.equipAttack  = s.get ("equip_attack", 5).asInt ();
      stats.equipDefense = s.get ("equip_defense", 2).asInt ();

      const int numPotions = spec.get ("potions", 3).asInt ();
      if (numPotions > 0)
        potions.push_back ({"health_potion", numPotions});

      for (const auto& g : spec["constraints"])
        {
          rog::Gate gate;
          gate.x = g.get ("x", 0).asInt ();
          gate.y = g.get ("y", 0).asInt ();
          gate.direction = g.get ("direction", "").asString ();
          constraints.push_back (gate);
        }
    }
  else
    ParseGameArgs (argc, argv, 2, seed, depth, hp, maxHp, stats, potions);

  const auto game = rog::PlayToGate (seed, depth, stats, hp, maxHp, potions,
                                     constraints, entryDir);

  Json::Value out (Json::objectValue);
  out["survived"] = game.HasSurvived ();
  out["xp"] = static_cast<Json::Int64> (game.GetTotalXp ());
  out["gold"] = static_cast<Json::Int64> (game.GetTotalGold ());
  out["kills"] = static_cast<Json::Int64> (game.GetTotalKills ());
  out["hp_remaining"] = game.GetPlayerHp ();
  out["exit_gate"] = game.GetExitGate ();
  out["actions"] = rog::ActionLogToJson (game.GetActionLog ());

  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  std::cout << Json::writeString (writer, out) << std::endl;

  return game.HasSurvived () ? 0 : 1;
}

} // anonymous namespace

int
main (int argc, char** argv)
{
  if (argc > 1 && std::string (argv[1]) == "--solve")
    return RunSolve (argc, argv);

  std::string seed;
  int depth, hp, maxHp;
  rog::PlayerStats stats;
  rog::DungeonGame::PotionList potions;
  ParseGameArgs (argc, argv, 1, seed, depth, hp, maxHp, stats, potions);

  auto game = rog::DungeonGame::Create (seed, depth, stats, hp, maxHp, potions);

  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";

  while (!game.IsGameOver ())
    {
      /* Output state.  */
      const auto state = GameStateToJson (game);
      std::cout << Json::writeString (writer, state) << std::endl;

      /* Read action.  */
      std::string line;
      if (!std::getline (std::cin, line))
        break;

      Json::Value input;
      Json::CharReaderBuilder reader;
      std::istringstream iss (line);
      std::string errs;
      if (!Json::parseFromStream (reader, iss, &input, &errs))
        {
          std::cerr << "Invalid JSON: " << errs << std::endl;
          continue;
        }

      const auto action = ParseAction (input);
      if (!game.ProcessAction (action))
        {
          Json::Value err (Json::objectValue);
          err["error"] = "Invalid action";
          std::cout << Json::writeString (writer, err) << std::endl;
        }
    }

  /* Final state.  */
  const auto finalState = GameStateToJson (game);
  std::cout << Json::writeString (writer, finalState) << std::endl;

  return game.HasSurvived () ? 0 : 1;
}
