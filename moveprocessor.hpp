#ifndef ROG_MOVEPROCESSOR_HPP
#define ROG_MOVEPROCESSOR_HPP

#include "moveparser.hpp"

#include <json/json.h>
#include <sqlite3.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rog
{

/**
 * Pro-rata pool split (SPEC_multiplayer_coop.md section 5a): each index
 * gets floor(pool * damages[i] / totalDamage); the leftover units go one
 * each to the largest remainders, ties to the lower index.  All-zero
 * damages yield all-zero shares.  Exact integer math, shared by the
 * settlement verification here and the client's claims computation.
 */
std::vector<int64_t> SplitPool (int64_t pool,
                                 const std::vector<int64_t>& damages);

/**
 * Processor for moves in confirmed blocks.  Validates via MoveParser
 * and then mutates the game-state database.
 */
class MoveProcessor : private MoveParser
{

private:

  /** Next visit ID (monotonic counter, deterministic across nodes).  */
  int64_t& nextVisitId;

  /** Moves the player to a segment coordinate.  */
  void SetPlayerSegment (const std::string& name, const SegmentKey& seg);

  /**
   * Records the bidirectional gate link between two neighbouring segments.
   * Each direction is inserted only if that (segment, direction) has no link
   * yet, so an existing link is never overwritten.
   */
  void LinkSegments (const SegmentKey& from, const std::string& fromDir,
                     const SegmentKey& to, const std::string& toDir);

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

  /**
   * Verifies and applies a dungeon settlement (XP/gold/loot credit,
   * visit closed, segment confirmed, death penalty if !survived) but
   * does NOT update the player's current_segment based on the replay's
   * exit gate.  The caller is responsible for setting position.
   *
   * Returns the replay's exit gate (empty string if the player died)
   * when the settlement was accepted, or std::nullopt if the replay
   * disagreed with the claimed results (move rejected, no state changes).
   *
   * Used by both ProcessExitChannel (which then sets position from the
   * replayed exit gate) and ProcessGateWalk (which uses the player's
   * declared direction to decide the destination).
   */
  std::optional<std::string> ApplySettlementBody (
      const std::string& name, int64_t visitId,
      const Json::Value& results, const Json::Value& actions);

  /**
   * Replay-verified outcome for one player of a settled run, as banked by
   * BankPlayerSettlement.  `lootDelta` is collected minus starting potions
   * (item id -> net quantity), applied to the inventory only on survival.
   */
  struct SettledOutcome
  {
    bool survived;
    int64_t xp;
    int64_t gold;
    int64_t kills;
    int64_t hpRemaining;
    std::string exitGate;
    std::map<std::string, int> lootDelta;
    std::vector<std::pair<int64_t, std::string>> finalInventory;
  };

  /**
   * Banks one player's replay-verified settlement outcome: final loadout
   * writeback, visit_results row, loot delta (on survival), the stats
   * update with death penalty and survival heal, death knock-back, and
   * XP/level-ups.  Shared by the solo path (ApplySettlementBody) and the
   * multiplayer path (ProcessSettle); does NOT touch the visit row or the
   * segment (the caller owns those, they are per-visit not per-player).
   */
  void BankPlayerSettlement (const std::string& name, int64_t visitId,
                             const SettledOutcome& outcome,
                             const SegmentKey& seg,
                             const std::string& entryDir);

  /**
   * Moves the player to the segment on the other side of `exitGate` from
   * `visitSeg`, which is simply the neighbouring coordinate.  No-op if that
   * cell holds no segment (and is not the hub).
   */
  void UpdatePositionFromExitGate (const std::string& name,
                                    const SegmentKey& visitSeg,
                                    const std::string& exitGate);

  /**
   * Deletes a provisional (confirmed=0) segment, along with its gates
   * and links.  No-op if the segment is confirmed.  Used in two
   * places:
   *
   *   - When a settlement is processed with survived=false (voluntary
   *     forfeit or force-settle timeout): the discoverer abandoned the
   *     attempt, so we free the world coord immediately instead of
   *     waiting ~300 blocks for the time-based pruner.  Prevents the
   *     "perpetual re-entry to hold a coord" griefing path.
   *   - The time-based pruner (in ProcessTimeouts) still runs, for
   *     segments that were never even entered.
   */
  void PruneProvisionalSegment (const SegmentKey& seg);

  /**
   * On death, relocate the player to the segment they came from (across the
   * gate they entered through) instead of teleporting to the hub, which would
   * be a free fast-travel exploit.  Opens a fresh solo run there, spawned at
   * that segment's gate facing the segment they died in.  No-op (leaving the
   * caller's hub default) when there is no such link, i.e. they died on the
   * first dive out of the hub.  The death penalty (half HP, gold loss) must
   * already be applied; this only relocates.
   */
  void RespawnAfterDeath (const std::string& name, const SegmentKey& diedSeg,
                          const std::string& entryDir);

protected:

  void ProcessRegister (const std::string& name) override;
  void ProcessDiscover (const std::string& name, int depth,
                         const std::string& txid,
                         const std::string& dir) override;
  void ProcessVisit (const std::string& name,
                      const SegmentKey& seg) override;
  void ProcessJoin (const std::string& name, int64_t visitId) override;
  void ProcessLeave (const std::string& name, int64_t visitId) override;
  void ProcessSettle (const std::string& name, int64_t visitId,
                       const Json::Value& results,
                       const Json::Value& actions) override;
  void ProcessSettleConfirm (const std::string& name, int64_t visitId,
                              const std::string& hash) override;
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
  void ProcessDiscardItem (const std::string& name, int64_t rowid) override;
  void ProcessEnterChannel (const std::string& name,
                             const SegmentKey& seg,
                             const std::string& entryDir) override;
  void ProcessExitChannel (const std::string& name,
                            int64_t visitId,
                            const Json::Value& results,
                            const Json::Value& actions) override;
  void ProcessGateWalk (const std::string& name,
                         const std::string& txid,
                         const std::string& dir,
                         const Json::Value& settlement) override;

public:

  MoveProcessor (sqlite3* d, unsigned height, int64_t& nextVisId)
    : MoveParser(d, height), nextVisitId(nextVisId)
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
