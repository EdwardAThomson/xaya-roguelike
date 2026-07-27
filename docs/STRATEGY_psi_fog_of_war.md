# Strategy: PSI Fog of War Between Unfriendly Players

_Status: design imported from the PSI_Cpp project, not yet integrated. Written 2026-07-27._

When two players who are not allies share a segment, each should learn only
"are we in sight of each other?" and never the other's actual position. This
document maps the working implementation and dispute design from the
[Private-Set-Intersection](https://github.com/EdwardAThomson/Private-Set-Intersection)
project onto this game's architecture. It is the multiplayer counterpart to
`docs/STRATEGY_action_proofs.md`: that document makes solo claims verifiable;
this one makes mutual hidden-information queries possible and auditable.

## What the PSI library provides today

The PSI_Cpp repo is a C++17 library plus tools, same toolchain as this GSP
(CMake, GoogleTest, libsodium):

- Tag-mode ECDH PSI on ristretto255: hash-to-group with unknown discrete log
  (the `H(x)*G` break is fixed and regression-tested), one-way BLAKE3
  membership tags, wire messages that carry no plaintext cells and leak no
  element lengths.
- Multi-level mesh cascade (coarse cells first, fine cells only where coarse
  intersected) and thread-parallel scalar ops. Benchmarks: 5,000 clustered
  cells per side in ~129 ms; a segment-sized set (tens of visible cells on an
  80x40 grid) is well under a millisecond of crypto per exchange.
- The commit-reveal dispute layer, phase 1 implemented
  (`docs/commit_reveal_spec.md` there): deterministic derivation of all
  protocol randomness from a per-turn seed, per-turn commitments that bind
  input set and seed, Ed25519-signed transcripts, and a `psi_audit` tool that
  recomputes every byte a party should have sent and reports
  `HONEST` or `FRAUD turn=.. level=.. dir=.. msgType=.. byteOffset=..`.

## Scope rule: unfriendly pairs only

PSI runs only between players who are not allies. PVE co-op teammates share
vision by consent, out of band, with no cryptography between them. The
protocol applies to PVP encounters and unallied strangers in a segment. A
room of `n` players therefore needs PSI only for its hostile pairs, not all
`n(n-1)/2` pairs, and most hostile encounters in this game are 1v1 or
party-vs-intruder.

Consequence for the rules layer: "hostile" must be a crisp, on-chain-visible
predicate (party membership, faction, truce flag), because the audit needs to
know whether a pair *should* have been exchanging PSI flights at a given turn.
The turn hostility begins is the turn PSI must begin; that boundary has to
come from committed game state, not from either player's claim. Ally-then-
betray transitions are the case to design carefully.

## Architecture fit

This game already has the two layers the PSI dispute spec calls its two
deployment profiles:

1. **Channelized sessions** (Layer 2 here): dungeon sessions already run in
   channels via libxayagame. A multiplayer session between two hostile
   parties carries PSI flights as additional channel message types; per-turn
   commitments become fields of the signed channel state. This is the
   "channelized 1v1" profile, and 2-party covers PVP duels and
   party-vs-intruder (one channel between the two sides).
2. **GSP as dispute arbiter** (Layer 1 here): the GSP already replays action
   proofs deterministically (`ApplySettlementBody`). The PSI audit is the same
   shape: deterministic recomputation from committed inputs, cheap (a few
   thousand scalar multiplications at most), and it slots into settlement or
   dispute handling as another validity predicate. No verification game
   (Truebit/Cartesi style) is needed because GSP computation is not
   gas-metered. Dispute evidence posted on-chain exposes only the disputed
   turn (the per-turn seed binding guarantees this).

Note the disclosure fact from the spec: on a small grid, a revealed blinding
scalar is equivalent to a revealed position (candidate cells can be tested
against the transcript). An 80x40 segment is exactly this regime, which is
why openings are confined to single disputed turns.

## Performance expectations

Per hostile pair per turn: padded visible-cell sets on an 80x40 segment are
small (N_max on the order of 64 to 256 cells), so the crypto is sub-millisecond
to low-millisecond, and the exchange pipelines (each side's phase-1 material
can be precomputed during the opponent's turn). The mesh cascade is likely
unnecessary at segment scale (single level suffices); it becomes relevant only
if PSI is ever run over multi-segment or overworld-scale sets.

## Phasing

1. **Duel PVP in a 2-party channel**: two hostile players in one segment, PSI
   flights as channel messages, deterministic derivation on, transcripts
   retained by both sides. Reuses the existing channel plumbing.
2. **Settlement/dispute integration**: GSP-side audit (link the PSI library,
   reuse its `audit` module) triggered on challenge; bonds and challenge
   deposits added to the channel close rules.
3. **Party-vs-intruder**: allies pool their visible-cell sets on one side of a
   single 2-party exchange (vision sharing among allies is by consent, so
   pooling is free).
4. **Deferred**: true n-party hostile brawls (needs either multiple pairwise
   exchanges per turn or a multiparty channel design), and any overworld-scale
   PSI (would want the mesh cascade).

## Open questions for this game

- The exact hostility predicate and its transition rules (see scope rule).
- N_max per segment (bounds both wire size and what set-size padding hides).
- Whether PSI transcripts ride inside the existing channel state format or as
  a parallel record referenced by it.
- Frontend parity: the TypeScript frontend would need the same protocol for
  local play against a live opponent; the sibling
  [psi-demo](https://github.com/EdwardAThomson/psi-demo) repo already holds a
  protocol-parity JS implementation (@noble/curves ristretto255, same tag and
  derivation constructions) that could seed that work.

## Provenance

The commit-and-replay dispute architecture is from
[Preventing cheaters in Fog Of War Games](https://edward-thomson.medium.com/preventing-cheaters-in-fog-of-war-games-69f202fbe107)
(2020), which this game's action-proof design already implements for solo
play. The PSI-transcript binding (seed-committed randomness, byte-exact audit)
follows a security review by the [xaya/fog-of-war](https://github.com/xaya/fog-of-war)
authors. Full lineage and the underlying protocol analysis live in the
PSI_Cpp repo: `docs/commit_reveal_spec.md`, `docs/dispute_resolution_notes.md`,
`docs/security_hardening.md`.
