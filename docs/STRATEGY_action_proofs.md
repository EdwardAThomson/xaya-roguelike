# Strategy: Action Replay Proofs at Scale

## The Problem

Every dungeon exit submits the complete action sequence as on-chain calldata.
This is the most expensive part of gameplay:

| Session length | Actions | JSON size | Calldata gas | Total gas | Polygon cost |
|---------------|---------|-----------|-------------|-----------|-------------|
| Quick run     | 20      | ~730 B    | ~12K        | ~130K     | ~$0.002     |
| Normal run    | 50      | ~1.7 KB   | ~25K        | ~146K     | ~$0.002     |
| Full clear    | 100     | ~3.3 KB   | ~50K        | ~170K     | ~$0.003     |
| Deep dungeon  | 200     | ~6.5 KB   | ~97K        | ~217K     | ~$0.003     |
| Extended play | 500     | ~16 KB    | ~240K       | ~360K     | ~$0.005     |

On Polygon at current prices (~30 gwei, MATIC ~$0.50) this is cheap. But:

1. **Gas limits**: The default 500K limit in xayax rejects sessions > ~100 actions
2. **Block gas limits**: Very long sessions could approach block limits
3. **If gas prices rise**: 10x gas price makes a 500-action session $0.05 per run
4. **Mainnet Ethereum**: Same calldata would cost ~$5-50 per session (not viable)

## Options

### Option A: Compact Action Encoding (Recommended short-term)

Replace verbose JSON with a compact string format:

```
Current:  [{"type":"move","dx":1,"dy":0},{"type":"pickup"},{"type":"wait"}]
Compact:  ["m1,0","p","w","u:health_potion","g"]
```

**Savings**: ~77% reduction in payload size. A 100-action session goes
from 3.3KB to ~750 bytes. Keeps the proof fully on-chain and verifiable.

**Implementation**: Add a compact parser in `ProcessExitChannel` alongside
the existing verbose one. Detect format by checking if the first element
is a string vs an object.

### Option B: Hash Commitment + Dispute Window (Recommended long-term)

Instead of submitting the full action proof on every exit, submit only a
hash of the proof. The full proof is available off-chain (stored by the
player). Anyone can challenge within a dispute window.

```
Exit move:  {"xc": {"id": N, "results": {...}, "proof_hash": "sha256..."}}
```

**Flow**:
1. Player exits channel, submits results + SHA-256 of the action log
2. GSP accepts provisionally (results applied immediately)
3. Challenge window opens (e.g. 100 blocks)
4. Anyone can submit `{"challenge": {"visit": N}}` during the window
5. If challenged, the player must reveal the full action proof within
   another window (e.g. 200 blocks)
6. If the proof doesn't match the hash, or the replay doesn't match the
   results, the player is penalized (results reversed + gold penalty)

**Savings**: Exit calldata drops to ~200 bytes regardless of session length.
Only challenged sessions require the full proof.

**Tradeoff**: More complex, requires a challenge/response protocol, and
honest players could lose rewards if they lose their action log before a
challenge resolves. Also requires economic stake for challengers to prevent
spam challenges.

### Option C: Xaya State Channels (Already Designed)

The libxayagame channel framework was designed for exactly this pattern.
In a proper state channel:

1. Both parties (player + game operator, or player + player) sign state
   updates off-chain
2. Only the final state goes on-chain (or a dispute proof)
3. The GSP already extends `ChannelGame` and has `BoardRules` implemented

**For solo channels**: The "other party" would be the game operator (or a
decentralized operator set). They co-sign each game state transition,
confirming it matches the rules.

**Savings**: Exit calldata is just a final signed state + signatures.
Constant size regardless of session length.

**Tradeoff**: Requires an online operator to co-sign state updates during
play. For solo dungeons this is lighter than multi-player, but still needs
operator infrastructure.

### Option D: ZK Proof (Future)

A zero-knowledge proof that the action sequence is valid and produces
the claimed results, without revealing the actions themselves.

**Savings**: Constant-size proof (~256 bytes) regardless of session length.
Also provides privacy (other players can't see your strategy).

**Tradeoff**: ZK circuit for the game engine would be extremely complex
to build and computationally expensive to generate. Not practical today
for a full roguelike game engine.

## Recommendation

**Phase 1 (now)**: Option A — compact encoding. Quick to implement, keeps
the existing architecture, reduces costs by 77%.

**Phase 2 (with multi-player)**: Option C — proper state channels via
libxayagame's channel framework. This is the intended architecture and
solves both the cost problem and the multi-player consensus problem.

**Phase 3 (if needed)**: Option B — hash commitment for any remaining
cases where full on-chain proof is too expensive but state channels are
overkill.

Skip Option D unless the game needs ZK privacy guarantees.
