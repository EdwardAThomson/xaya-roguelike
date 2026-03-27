/**
 * Interactive dungeon play binary.  Communicates via JSON on stdin/stdout.
 *
 * Usage: roguelike-play [seed] [depth] [hp] [maxhp]
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
 */

#include "dungeongame.hpp"
#include "dungeon.hpp"
#include "combat.hpp"
#include "items.hpp"

#include <json/json.h>

#include <cstdlib>
#include <iostream>
#include <string>

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

int
main (int argc, char** argv)
{
  const std::string seed = argc > 1 ? argv[1] : "default_seed";
  const int depth = argc > 2 ? std::atoi (argv[2]) : 1;
  const int hp = argc > 3 ? std::atoi (argv[3]) : 100;
  const int maxHp = argc > 4 ? std::atoi (argv[4]) : 100;

  const int level = argc > 5 ? std::atoi (argv[5]) : 1;

  rog::PlayerStats stats;
  stats.level = level;
  stats.strength = 10;
  stats.dexterity = 10;
  stats.constitution = 10;
  stats.intelligence = 10;
  stats.equipAttack = 5;   /* short sword */
  stats.equipDefense = 2;  /* leather armor */

  rog::DungeonGame::PotionList potions = {{"health_potion", 3}};
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
