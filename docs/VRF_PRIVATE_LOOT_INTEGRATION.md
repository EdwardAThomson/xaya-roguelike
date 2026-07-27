# VRF Private Loot Integration

## Overview

This document designs the integration of player-held-VRF private loot (the
pattern prototyped in `~/Projects/vrf-loot-generator`, see its
`docs/BLOCKCHAIN_ROGUELIKE_ARCHITECTURE.md`) into this GSP. The goal: dungeon
loot that is private to the player until they choose to reveal it, yet
provably fair. The chain stores only anchors, keys, and ownership; item data
lives on the player's machine.

Source-side decisions already made (carried over unchanged):

- RFC 9381 ECVRF, edwards25519 suite (ECVRF-EDWARDS25519-SHA512-TAI)
- Dedicated VRF keypair derived from a wallet signature over a
  domain-separated message (not the wallet key itself)
- Fixed-width VRF input: `anchor (32 bytes) || index (4 bytes, big-endian)`
- The VRF public key must be on-chain strictly before the seed exists

The core tension: today the GSP knows all loot. Ground items and monster
drops are recomputed by deterministic replay from the public segment seed
(`dungeongame.cpp`, `SpawnGroundItems`, seeded `HashSeed(seed + ":game:" + depth)`),
and inventory lives in the on-chain `inventory` table. VRF loot inverts this:
the GSP cannot see unrevealed items. The design below resolves this by making
VRF loot a **new, parallel loot category** that never enters the replay path,
and by converting items into ordinary on-chain inventory rows **at reveal
time**.

---

## Two Loot Layers

| Layer | Source | Visibility | Validated by |
|---|---|---|---|
| Public loot (existing) | Session RNG from public segment seed | Public | Action-replay in `ApplySettlementBody` |
| VRF private loot (new) | `ECVRF(player_sk, anchor \|\| i)` | Private until reveal | ECVRF proof check in the reveal move |

Public loot is unchanged in phase 1. Private loot is a bonus layer awarded
per successfully settled visit. Later phases may migrate categories of drops
from public to private.

---

## 1. The Seed Anchor

### What plays the role of tx_hash

The natural candidate is the txid of the `ec` (enter channel) move, which
the GSP already receives per move (`moveparser.cpp` `ProcessOne` reads
`mvid`/`txid`; discovery already uses it as the segment seed). But a txid
alone is a **bad** VRF seed here: the player constructs and signs the
transaction, so they know its hash before broadcasting. With a fixed
registered `sk`, they could compute the loot for candidate txids offline and
only broadcast a favourable one. That is zero-cost input grinding, worse than
the fee-gated grinding the source architecture worries about.

Therefore the anchor mixes in per-block entropy the player cannot know at
signing time:

```
anchor = SHA-256( txid_of_ec_move || block_rngseed_of_including_block )
```

- `txid` binds the anchor to this player's specific entry.
- `block_rngseed` is unknown until the block containing the tx is produced,
  so pre-broadcast grinding is impossible. The player cannot choose which
  block includes their tx.

libxayagame populates `blockData["block"]` with `hash`, `rngseed`, etc.; the
GSP currently reads only `height` in `RoguelikeLogic::UpdateState`
(`logic.cpp`). The change is to also read the rngseed and thread it through
the `MoveProcessor` constructor alongside `height`.

**Needs confirmation**: that Xaya X (protocol 2, EVM backend) actually
supplies a meaningful `rngseed` in `blockData` rather than an empty field.
If it does not, fall back to `block.hash`. Either way the value is chosen by
the block producer, so a Polygon validator could in principle bias it. The
bias cost (reordering or withholding blocks) vastly exceeds the value of a
game item, so this is accepted, same as the source architecture accepts
miner bias on tx inclusion. Document it; do not engineer around it yet.

### Recording the anchor

The GSP computes the anchor while processing `ec` and stores it on the visit:

```sql
ALTER TABLE visits ADD COLUMN vrf_anchor TEXT NULL;   -- hex, 32 bytes
ALTER TABLE visits ADD COLUMN vrf_key    TEXT NULL;   -- pubkey snapshot, hex
```

Snapshotting the key on the visit pins which key generates this visit's
loot, which makes rotation rules trivial (see below).

### Item count and difficulty

Both must be publicly derivable so any verifier can bound the loot:

```
item_count = (SHA-256(anchor)[31] % MAX_VRF_ITEMS) + 1     (MAX_VRF_ITEMS = 8)
LootMap input parameters: depth of the segment (public, from `segments`)
```

Depth-gating rarity happens inside `LootMap(digest, depth)` by shifting
rarity thresholds, exactly as the source doc's open question 5 suggests.

---

## 2. Key Registration Move

New move type, following the existing single-letter convention:

```
{"vk": {"key": "<64 hex chars>"}}
```

### Schema

```sql
CREATE TABLE IF NOT EXISTS `vrf_keys` (
  `name`         TEXT NOT NULL,
  `pubkey`       TEXT NOT NULL,          -- 32-byte edwards25519 point, hex
  `valid_from`   INTEGER NOT NULL,       -- block height of registration
  `valid_until`  INTEGER NULL,           -- set when rotated, else NULL
  PRIMARY KEY (`name`, `valid_from`)
);
```

### Validation rules

| Rule | Enforcement |
|---|---|
| Player exists | Same precondition style as other moves in `MoveParser` |
| Key is a valid curve point | Decode check at parse time; reject junk |
| One active key per player | Registering while a key is active rotates: old row gets `valid_until = height`, new row inserted |
| Ordering vs seed | `ec` requires an active key with `valid_from < ec_block_height` (strictly less). A key registered in the same block as the `ec` move does not count. |

The strict ordering closes the keypair-grinding hole: a player must commit
to `pk` before any anchor derived from their entry can exist. Same-block
registration is excluded because within one block the player controls both
transactions and (depending on ordering rules) could react to the rngseed.

### Rotation

Rotation is just re-registration. Because each visit snapshots `vrf_key`,
old unrevealed loot stays verifiable against the key that was active at
entry; the `vrf_keys` history table lets any verifier confirm that key was
active at that height. Rotation cannot retroactively re-roll loot, since
anchors are already fixed. No cooldown is needed, but a modest one
(reuse `DISCOVERY_COOLDOWN`) is cheap insurance against rotation spam.

Client-side, the keypair is derived deterministically from a wallet
signature over a domain-separated message, so "rotation" should be rare
(essentially only on suspected compromise):

```
sk = SHA-512( wallet_sign("xaya-roguelike-vrf|v1|" + player_name + "|" + chain_id) )[clamped]
```

---

## 3. Lifecycle

```
  Player / Client                        Chain / GSP
  ───────────────                        ───────────

  1. {"vk": {key}}  ────────────────►    vrf_keys row (valid_from = h0)

  2. {"ec": {id}}   ────────────────►    anchor = SHA-256(txid || rngseed)
                                         visits.vrf_anchor, visits.vrf_key set
                                         (requires valid_from < h1)
                    ◄────────────────    anchor readable via RPC

  3. Play dungeon off-chain (unchanged: channel, action log)

  4. Locally, privately:
     for i in 0..item_count:
       (O_i, pi_i) = ECVRF(sk, anchor || i)
       item_i = LootMap(SHA-256(O_i), depth)
     Store (O_i, pi_i, item_i) in IndexedDB.
     Items are PENDING until the visit settles.

  5. {"xc": {id, results, actions}} ─►   replay verified (unchanged)
                                         on survival: visit settled,
                                         private loot slots become CLAIMABLE

  6. {"rv": {visit, i, o, pi}} ─────►    GSP verifies ECVRF proof,
     (only when the player wants         re-derives item via LootMap,
      the item on-chain)                 inserts inventory row
                                         Item is now ordinary public inventory.

  7. Equip / use / trade  ──────────►    existing moves (eq, ui, ...)
```

### Reveal-or-abandon, resolved by settlement gating

Private loot for a visit is only claimable if that visit **settled with
`survived = true`** via the existing replay-verified `xc`/`gw` path. This is
the single most important coupling in the design, because it makes the
existing game loop the anti-grinding mechanism:

- Peeking is allowed and harmless: the player can compute their loot the
  moment the anchor exists. But turning it into claimable loot requires
  actually completing the dungeon run, which costs real play time plus the
  gas of `ec` and `xc`.
- Abandoning a bad roll costs a full entry: the timeout force-settle
  (`SOLO_VISIT_ACTIVE_TIMEOUT`, death outcome) never yields claimable slots,
  and re-entering creates a new visit, new txid, new block, new anchor.
- There is nothing to "abandon into": unrevealed loot from an unsettled
  visit is simply invalid, so the economics degrade to "grind = replay the
  dungeon", which is just playing the game.

---

## 4. What the GSP Can and Cannot Validate

| Question | Answer |
|---|---|
| Does this visit have private loot? | Yes: anchor + item_count are public. |
| What are the items? | No, not until reveal. The GSP tracks only which `(visit, i)` slots are unrevealed vs revealed. |
| Can an unrevealed item affect combat, stats, or replay? | Must not. Unrevealed items grant nothing. Stats apply only after reveal (reveal-on-equip is therefore automatic: revealing inserts the inventory row, equipping is the existing `eq` move). |
| Does the action replay reference private loot? | No. Private loot lives entirely outside `DungeonGame`. The replay's `pickup` actions still reference public ground items only. `ApplySettlementBody` is untouched except for flipping slots to CLAIMABLE on success. |
| Can a player double-reveal or reveal someone else's slot? | No: reveal is keyed on `(visit_id, index)`, checked against visit ownership and a revealed-flag. |

A later phase can move dungeon drops themselves into the private layer. Then
pickups would reference opaque slot tokens ("picked up chest 3"), the replay
validates position and sequence but not content, and slot index 3 maps to
VRF index 3. That works because the replay never needed item identity to
verify movement and combat, but it does change drop-driven gameplay (e.g.
picking up a potion mid-run and drinking it is impossible if its identity is
private), so v1 deliberately keeps in-run items public.

---

## 5. Reveal and Trade Moves

### Reveal

```
{"rv": {"visit": N, "i": 3, "o": "<vrf output, hex>", "pi": "<80-byte proof, hex>"}}
```

GSP processing:

1. Visit exists, `initiator == player`, settled, `survived = 1`, has anchor.
2. `i < item_count(anchor)`; slot `(visit, i)` not already revealed.
3. `ECVRF_verify(visits.vrf_key, anchor || uint32be(i), pi)` yields `O'`;
   require `O' == o`.
4. `item = LootMap(SHA-256(o), depth)`; insert into `inventory`
   (respecting `MAX_INVENTORY`; if the bag is full, reject the move so the
   slot is not burned).
5. Record the reveal:

```sql
CREATE TABLE IF NOT EXISTS `vrf_reveals` (
  `visit_id` INTEGER NOT NULL,
  `idx`      INTEGER NOT NULL,
  `name`     TEXT NOT NULL,           -- owner at reveal time
  `height`   INTEGER NOT NULL,
  PRIMARY KEY (`visit_id`, `idx`)
);
```

Note on cost: unlike an EVM contract, ECVRF verification in the GSP costs no
gas. Every GSP instance runs it deterministically as part of move
processing; the on-chain cost is only the calldata of the move (~120 bytes
of proof material). This is a genuine advantage of the Xaya model over the
source repo's contract-based framing.

### C++ ECVRF verification options

- **libsodium upstream**: no VRF API. Not an option as-is.
- **Algorand's libsodium fork** (`crypto_vrf_*`): battle-tested, but
  implements draft-irtf-cfrg-vrf-03 semantics, which differ from final
  RFC 9381 in hash-to-curve and validation details. Usable only if the TS
  side matches the same draft. Needs confirmation of current fork status.
- **Vendored RFC 9381 implementation** (recommended): a small
  ECVRF-EDWARDS25519-SHA512-TAI verify-only implementation over libsodium's
  ed25519 core ops, vendored into the repo, pinned, and tested against the
  RFC 9381 Appendix B test vectors. Verification is ~200 lines. This is
  consensus-critical code: any divergence between GSP builds is a fork, so
  vendor-and-pin beats linking a moving external library.

### Trade

Trading is **revealed-trade only**. Selling an unrevealed slot would require
handing over `sk` (or the buyer trusting an unverifiable claim), and leaking
`sk` leaks every item from every visit. So:

1. Reveal converts the item into an ordinary `inventory` row.
2. Trade is then a normal inventory transfer, a future move type independent
   of this design (no player-to-player inventory transfer exists in the GSP
   today). `vrf_reveals` plus the anchor gives permanent provenance for any
   revealed item.
3. Private (unrevealed) transfer via zero-knowledge proofs ("I own a valid
   slot whose item has rarity >= X") is a possible future layer, explicitly
   out of scope. `docs/STRATEGY_action_proofs.md` Option D notes ZK is not
   practical for this project today; the same verdict applies here.

The counterparty-verification flow from the source architecture still works
off-chain for showing items before a trade: anchor and pubkey are readable
from GSP RPC, so any client can verify a peer's revealed `(o, pi)` without
any move at all. Only ownership transfer needs the chain.

---

## 6. Client-Side Responsibilities (TS frontend)

Current state (from `~/Projects/xaya-roguelike-frontend`): no local
persistence at all, no key handling (proxy transport signs server-side;
`walletTransport.ts` is a stub), and the client never sees txids of its own
moves (fire-and-forget + state re-polling).

| Responsibility | Design |
|---|---|
| VRF key derivation | Requires a real wallet signature, so this depends on finishing `WalletMoveTransport`. On devnet/proxy mode, derive from a locally generated secret instead (clearly non-custodial-demo quality). |
| Key storage | `sk` in IndexedDB, plus deterministic re-derivation from the wallet as recovery. Losing `sk` with no wallet-derivation path means losing all unrevealed loot; this is why derivation, not random generation, is the rule. |
| Anchor discovery | The client does not need the txid: it polls the GSP for `visits.vrf_anchor` after `ec` confirms (extend `getplayerinfo` / `getsegmentinfo` state JSON). This fits the existing no-txid transport model. |
| Loot generation and storage | Compute all `(O_i, pi_i, item_i)` when the anchor appears; persist in IndexedDB keyed by `(visit_id, i)` with status pending / claimable / revealed. |
| LootMap parity | `LootMap` must be bit-for-bit identical in C++ and TS, same discipline as dungeon generation (`hash.ts` / `rng.ts` / Lemire `nextInt` already prove the pattern). LootMap should use only SHA-256 byte extraction, no MT19937, to keep the parity surface minimal. Add parity vectors next to `parity_test.ts`. |
| ECVRF in TS | Needs a library decision: no obvious maintained RFC 9381 TS implementation is known to be a drop-in (noble-curves has the primitives but not the VRF construction). Budget for porting/vendoring one, tested against the same RFC vectors as the C++ side. Prove/verify both needed client-side. |

The Python AI player either skips private loot entirely (it is optional
bonus loot, nothing breaks) or gains a minimal ECVRF prover later.

---

## 7. Economics

What entry costs exist today:

| Cost | Exists? | Notes |
|---|---|---|
| In-game fee per move | No | No CHI/WCHI burn or gold cost anywhere in `logic.cpp` / `moveprocessor.cpp` |
| L1 gas per move | Yes | Polygon calldata cost per `ec`/`xc`; small but nonzero |
| Time cost | Yes | A settled, survived run is required for claimable loot |
| Cooldowns | Yes | `DISCOVERY_COOLDOWN` = 50 blocks; channel timeouts |

Verdict: gas alone would not deter grinding on Polygon, but the settlement
gate changes the unit of grinding from "one transaction" to "one completed
dungeon run with a verified action replay". Combined with the rngseed-mixed
anchor (no pre-broadcast peeking), the existing mechanics are judged
sufficient for launch. If loot value ever grows enough that botting full
runs pays, add a gold entry fee to `ec` (a one-line precondition plus
deduction), which also gives gold a sink. Death already burns 25% of gold,
so a failed grind run is mildly negative even before fees.

---

## 8. Phased Adoption Path

### Phase V0: Crypto foundations (no gameplay change)

- [ ] Vendor C++ ECVRF verify (RFC 9381 TAI suite) + RFC test vectors
- [ ] TS ECVRF prove/verify + identical vectors
- [ ] Define `LootMap(digest, depth)` in C++ and TS + parity tests
- [ ] Confirm `blockData["block"]["rngseed"]` content under Xaya X protocol 2

Effort: 1-2 weeks. Mostly careful porting and test vectors.

### Phase V1: Keys and anchors on-chain

- [ ] `vk` move, `vrf_keys` table, rotation rules
- [ ] Thread rngseed through `UpdateState` into `MoveProcessor`
- [ ] Compute and store `vrf_anchor` / `vrf_key` on `ec` (and the `gw` enter path)
- [ ] Expose anchor + keys in state JSON / RPC
- [ ] Frontend: key derivation (proxy-mode variant), IndexedDB persistence

Effort: ~1 week. Schema change requires a devnet reset (pre-launch, fine).

### Phase V2: Reveal (smallest useful increment)

- [ ] Claimable-on-settlement gating in `ApplySettlementBody`
- [ ] `rv` move with ECVRF verification, `vrf_reveals` table, inventory insert
- [ ] Frontend: local loot generation, pending/claimable/revealed UI, reveal flow
- [ ] Security tests: wrong proof, wrong key epoch, index out of range,
      unsettled visit, double reveal, full bag

Effort: 1-2 weeks. After V2 the feature is player-visible and complete for
solo play.

### Phase V3: Trade and provenance

- [ ] Player-to-player transfer move for inventory rows (new mechanic)
- [ ] Off-chain peer verification helper in the frontend (show-before-trade)

Effort: ~1 week, mostly the transfer move design (escrow vs naive give).

### Phase V4+ (backlog, unscheduled)

- Migrate dungeon drops to opaque private slots (replay references tokens)
- Gold entry fee if grinding becomes profitable
- ZK proofs for unrevealed-item claims
- On-chain destruction of revealed VRF items (existing `di` already covers it)

---

## Open Questions

1. **Xaya X rngseed**: is `block.rngseed` populated and non-degenerate when
   libxayagame runs against Xaya X on an EVM chain, and what is it derived
   from there? Must be answered before V1; fallback is `block.hash`.
2. **Draft vs final ECVRF**: if the Algorand fork is chosen for C++, the TS
   side must implement the same draft. Decide once library evaluation is
   done in V0; default is vendored RFC 9381 on both sides.
3. **Multiplayer visits**: `v`/`j` visits have multiple participants. Does
   each participant get their own private loot from a per-participant anchor
   (their `j` txid), or is private loot solo-only at first? Proposal:
   solo-only (`ec` path) until multiplayer settlement itself is finished.
4. **Wallet transport**: real key derivation needs `WalletMoveTransport`
   completed (it currently throws). Sequencing dependency, not a design one.
5. **Anchor for `gw`**: the gate-walk move settles and enters atomically;
   the new visit's anchor comes from the `gw` txid + rngseed, but confirm
   the ordering inside `ProcessGateWalk` leaves no same-block registration
   loophole.
6. **Reveal spam**: `MAX_VRF_ITEMS = 8` bounds reveals per visit, but a
   per-block or per-move-count sanity cap on `rv` may be worth adding since
   verification runs on every GSP node.
