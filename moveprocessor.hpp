#ifndef ROG_MOVEPROCESSOR_HPP
#define ROG_MOVEPROCESSOR_HPP

#include "moveparser.hpp"

#include <json/json.h>
#include <sqlite3.h>

#include <cstdint>
#include <string>

namespace rog
{

/**
 * Processor for moves in confirmed blocks.  Validates via MoveParser
 * and then mutates the game-state database.
 */
class MoveProcessor : private MoveParser
{

private:

  /** Next segment ID (simple counter, deterministic across nodes).  */
  int64_t& nextSegmentId;

  /** Next visit ID (simple counter, deterministic across nodes).  */
  int64_t& nextVisitId;

  /**
   * Inserts the starting items for a newly registered player.
   */
  void GiveStartingItems (const std::string& name);

  /**
   * Returns the number of participants currently in a visit.
   */
  int64_t CountParticipants (int64_t visitId);

  /**
   * Returns the max_players for a visit (from its parent segment).
   */
  int64_t GetMaxPlayers (int64_t visitId);

  /**
   * Recalculates max_hp from base constitution + equipment bonuses.
   * Called after equip/unequip to keep HP in sync with gear changes.
   */
  void RecalcMaxHp (const std::string& name);

protected:

  void ProcessRegister (const std::string& name) override;
  void ProcessDiscover (const std::string& name, int depth,
                         const std::string& txid,
                         const std::string& dir) override;
  void ProcessVisit (const std::string& name,
                      int64_t segmentId) override;
  void ProcessJoin (const std::string& name, int64_t visitId) override;
  void ProcessLeave (const std::string& name, int64_t visitId) override;
  void ProcessSettle (const std::string& name, int64_t visitId,
                       const Json::Value& results) override;
  void ProcessAllocateStat (const std::string& name,
                             const std::string& stat) override;
  void ProcessTravel (const std::string& name,
                       const std::string& dir,
                       const std::string& txid) override;
  void ProcessUseItem (const std::string& name,
                        const std::string& itemId) override;
  void ProcessEquip (const std::string& name,
                      int64_t rowid, const std::string& slot) override;
  void ProcessUnequip (const std::string& name, int64_t rowid) override;
  void ProcessEnterChannel (const std::string& name,
                             int64_t segmentId) override;
  void ProcessExitChannel (const std::string& name,
                            int64_t visitId,
                            const Json::Value& results,
                            const Json::Value& actions) override;

public:

  MoveProcessor (sqlite3* d, unsigned height,
                 int64_t& nextSegId, int64_t& nextVisId)
    : MoveParser(d, height),
      nextSegmentId(nextSegId), nextVisitId(nextVisId)
  {}

  /** Blocks before an open visit expires (not enough players joined).  */
  static constexpr unsigned VISIT_OPEN_TIMEOUT = 100;

  /** Blocks before a solo active visit force-settles.  */
  static constexpr unsigned SOLO_VISIT_ACTIVE_TIMEOUT = 200;

  /** Blocks before a multiplayer active visit force-settles.  */
  static constexpr unsigned VISIT_ACTIVE_TIMEOUT = 1000;

  /** Cooldown blocks between segment discoveries.  */
  static constexpr unsigned DISCOVERY_COOLDOWN = 50;

  /** HP formula constants.  */
  static constexpr int BASE_HP = 50;
  static constexpr int HP_PER_CON = 5;

  /** Health potion heal amount.  */
  static constexpr int POTION_HEAL = 25;

  /** Random encounter constants for overworld travel.  */
  static constexpr int ENCOUNTER_CHANCE = 20;
  static constexpr int ENCOUNTER_MIN_DMG = 5;
  static constexpr int ENCOUNTER_MAX_DMG = 15;

  MoveProcessor () = delete;
  MoveProcessor (const MoveProcessor&) = delete;
  void operator= (const MoveProcessor&) = delete;

  /**
   * Processes all moves from a block, then runs timeout processing.
   */
  void ProcessAll (const Json::Value& moves);

  /**
   * Expires open visits that have been waiting too long and
   * force-settles active visits that have exceeded the active timeout.
   * Called automatically at the end of ProcessAll.
   */
  void ProcessTimeouts ();

};

} // namespace rog

#endif // ROG_MOVEPROCESSOR_HPP
