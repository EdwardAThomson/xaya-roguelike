# Security Model — Attack Vectors & Mitigations

## Overview

This document covers known attack vectors against the blockchain roguelike
and the mitigations in place or planned. The game runs on an EVM chain via
Xaya X, with game logic processed by a deterministic GSP.

---

## 1. Fabricated Dungeon Results

**Attack**: Player claims XP/gold/loot they didn't earn by submitting
false results in the `"xc"` (exit channel) move.

**Mitigation**: Action replay verification. The `"xc"` move requires the
player's complete action sequence. The GSP replays the actions on a fresh
DungeonGame from the segment seed. Claimed results must match the replay
exactly — mismatches are rejected. The deterministic dungeon (SHA-256 seed
→ MT19937 RNG → identical layout) makes fabrication impossible.

**Status**: Implemented.

---

## 2. World Map Pollution (Garbage Segments)

**Attack**: Player rapidly discovers segments with no intention of completing
them, polluting the world graph with provisional segments that block other
players.

**Mitigations**:
- **Discovery cooldown**: 50 blocks (~1.5 min on Polygon) between discoveries.
  Legitimate players spend 5-10 minutes per dungeon, so this is invisible to
  honest play but makes spam expensive and slow.
- **Provisional segments**: A discovered segment is provisional until the
  discoverer completes a valid run. If the run times out or fails validation,
  the segment is pruned from the world graph entirely (segment, gates, and
  links all deleted).
- **Gas cost**: Each discovery is an on-chain transaction. Spamming costs real
  money.

**Status**: Cooldown and provisional segments — implementing.

---

## 3. Channel Griefing (Blocking Segments)

**Attack**: Player enters a channel for a segment and never completes it,
blocking other players from visiting that segment.

**Mitigations**:
- **Solo channel timeout**: 200 blocks (~6 minutes). If the channel isn't
  settled within this window, the visit is force-settled with no rewards and
  the player receives a death.
- **Active visit limit**: Only one active visit per segment at a time. Once
  the timeout fires, the segment is free for others.
- **Cooldown after channel exit**: Prevents rapid re-entry to grief the same
  segment repeatedly.

**Status**: Timeouts exist (adjusting values). Cooldown — implementing.

---

## 4. Cross-Instance Replay

**Attack**: Submit moves from one game instance to another (different
deployment on the same chain, or different chain entirely).

**Mitigations**:
- **dungeon_id**: Each game instance has a unique `--dungeon_id` mixed into
  all segment seeds. Different dungeon IDs → different dungeon layouts →
  action replays fail verification.
- **Chain isolation**: Different EVM chains have different XayaAccounts
  contracts. Transactions can't cross chains.
- **Segment seeds from txid**: Each segment's seed includes the transaction
  hash, which is unique per chain and per deployment.

**Status**: Implemented.

---

## 5. Provisional Segment Travel Race

**Attack**: Alice discovers a provisional segment. Bob travels to it before
Alice completes her run. Alice's run fails/times out → segment is pruned.
Bob is now stranded at a non-existent segment.

**Mitigations**:
- **Block travel to provisional segments**: Players cannot travel to a segment
  that hasn't been validated yet. The `"t"` move should check that the
  destination segment is confirmed (not provisional).
- **Discoverer exclusivity**: Only the discoverer can be in the provisional
  segment. Others must wait for confirmation.

**Status**: Planned — implementing with provisional segments.

---

## 6. Front-Running Segment Discovery

**Attack**: Alice broadcasts a discover transaction. Bob sees it in the
mempool and submits his own discover in the same direction, hoping his gets
mined first.

**Mitigations**:
- **Transaction ordering**: On EVM chains, transaction ordering within a block
  is determined by the block producer (typically by gas price). This is a
  general blockchain problem, not specific to our game.
- **Different seeds**: Even if both discover the same direction, each gets a
  different seed (from their txid), producing a different dungeon.
- **Direction locking**: The `segment_links` table prevents duplicate links.
  The first transaction to be processed wins; the second is rejected.

**Status**: Partially mitigated by existing checks. Full front-running
protection would require commit-reveal or similar schemes.

---

## 7. Modified Frontend

**Attack**: Player modifies the frontend to reveal the full map (ignoring FOV),
see through walls, or show monster positions.

**Mitigations**:
- **Not a real threat**: The dungeon is generated from a known seed. A
  determined player could always regenerate the full map from the seed.
  FOV is a gameplay experience feature, not a security boundary.
- **Anti-cheat is server-side**: All results are verified by action replay.
  Seeing the map doesn't let you fabricate results.

**Status**: Accepted risk. FOV is cosmetic, not security.

---

## 8. Transaction Spam / Denial of Service

**Attack**: Flood the chain with invalid moves to slow down the GSP or
bloat the database.

**Mitigations**:
- **Gas cost**: Every transaction costs gas. Spamming is expensive.
- **Move validation**: Invalid moves are rejected early in the parser
  (HandleOperation) before any database writes. Minimal processing cost.
- **Existing chain protections**: EVM chains have block gas limits that
  naturally throttle transaction volume.

**Status**: Mitigated by blockchain economics and early validation.

---

## 9. Inventory Manipulation

**Attack**: Exploit the channel exit to add items beyond the inventory limit,
or duplicate items.

**Mitigations**:
- **Inventory limit (MAX_INVENTORY=20)**: Enforced on channel exit and
  settlement. Excess items are dropped.
- **Replay verification**: Loot comes from the replay, not from claims.
  Can't fabricate items that the replay didn't produce.
- **Item consumption tracking**: Potions used during the dungeon session
  are consumed from the starting inventory. The replay tracks this.

**Status**: Implemented.

---

## 10. Block Timing Attacks

**Attack**: Manipulate block timestamps to affect randomness or timeouts.

**Mitigations**:
- **Block-based timeouts**: All timeouts use block counts, not timestamps.
  Block counts are monotonically increasing and consensus-agreed.
- **Seed from txid**: Random seeds come from transaction hashes, not
  timestamps. Miners can't predict transaction hashes.

**Status**: Mitigated by design.

---

## Constants

| Parameter | Value | Purpose |
|-----------|-------|---------|
| VISIT_OPEN_TIMEOUT | 100 blocks | Open visits expire |
| VISIT_ACTIVE_TIMEOUT | 200 blocks (solo) | Solo channel timeout |
| DISCOVERY_COOLDOWN | 50 blocks | Between segment discoveries |
| MAX_INVENTORY | 20 | Inventory size limit |
| ENCOUNTER_CHANCE | 20% | Random encounters during travel |

---

## Future Considerations

- **Multi-player channels**: Require full state signing and dispute resolution
  (Phase 14). The OpenChannel framework from libxayagame handles this.
- **VRF-based loot**: Verifiable Random Function for private loot generation
  that's provably fair but hidden until revealed.
- **Boss instances / PvP**: Inside channels, require careful design around
  state agreement between multiple parties.
- **Economic attacks**: Gold inflation, market manipulation, item duplication.
  Need economic modeling before deploying with real value.
