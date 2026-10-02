#ifndef ROG_DUNGEONAI_HPP
#define ROG_DUNGEONAI_HPP

#include "dungeongame.hpp"
#include "dungeon.hpp"

#include <json/json.h>

#include <string>
#include <utility>
#include <vector>

namespace rog
{

/**
 * BFS first-step from (fromX,fromY) toward (toX,toY) over non-wall tiles,
 * navigating around walls.  Returns the (dx,dy) of the first step, or
 * (0,0) if no path exists.
 */
std::pair<int, int> BfsStepToward (const DungeonGame& game,
                                   int fromX, int fromY, int toX, int toY);

/**
 * Drives a fresh DungeonGame (same seed/depth/stats/hp/potions the GSP
 * will replay with) to the nearest gate — quaffing potions when low and
 * auto-attacking any monster blocking the path — and returns the
 * resulting game.  Its recorded action log (game.GetActionLog ()) is a
 * valid winning proof: feeding it back through DungeonGame::Replay (what
 * the GSP does) reproduces this exact survived=true outcome.
 *
 * Tooling knobs (none affects what the replay accepts): `exitDir` heads
 * for that gate instead of the nearest one, so a test can choose which
 * side of the segment the player comes out on; `collectGold` detours to
 * every reachable pile of gold coins first, so a test can earn gold.
 */
DungeonGame PlayToGate (const std::string& seed, int depth,
                        const PlayerStats& stats, int hp, int maxHp,
                        const DungeonGame::PotionList& potions,
                        const std::vector<Gate>& constraints = {},
                        const std::string& entryDir = "",
                        const std::string& exitDir = "",
                        bool collectGold = false);

/** Serializes a dungeon action log into the JSON proof the xc move carries. */
Json::Value ActionLogToJson (const std::vector<Action>& actions);

/**
 * Serializes a multiplayer merged log into the verbose JSON an `s` settle
 * carries: each entry is ActionLogToJson's object plus "i", the actor's
 * canonical index.
 */
Json::Value MergedLogToJson (const std::vector<LoggedAction>& merged);

} // namespace rog

#endif // ROG_DUNGEONAI_HPP
