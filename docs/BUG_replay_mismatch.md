# Bug: Channel Exit Silently Reverts for Large Action Proofs

## Summary

Channel exit moves (`xc`) with large action replay proofs (~50+ actions,
~2KB+ JSON) silently revert on the EVM because the `XayaAccounts.move()`
smart contract call exceeds the 500K gas limit hardcoded in `xayax/eth.py`.

The move transaction is submitted and returns a txid, but reverts on-chain.
The GSP never receives the move. No error is surfaced to the caller.

## Status

**Fixed** for devnet scripts (increased gas limit in `ai_explorer.py` and
`frontend_devnet.py` for large moves). See "Long-term Strategy" below for
production considerations.

## Symptoms

- `ProcessExitChannel` logs: `Channel exit REJECTED: claimed results do not
  match replay` (when GSP WARNING log is captured)
- Player remains `in_channel=true` after submitting exit
- The claimed `xp`, `gold`, or `kills` differ from the GSP's replay
- Always reproducible with the same seed — the same seed fails every time,
  different seeds may pass or fail

## Steps to Reproduce

### Quick reproduction

```bash
source ~/Explore/xayax/.venv/bin/activate
cd ~/Projects/xayaroguelike

python3 -c "
from xayax.eth import Environment
import json, jsonrpclib, os, shutil, subprocess, time, glob, importlib.util
os.environ['PATH'] = os.path.expanduser('~/.foundry/bin') + ':' + os.environ['PATH']
GAME_ID = 'rog'

for attempt in range(10):
    basedir = '/tmp/rog_repro_%d' % attempt
    shutil.rmtree(basedir, True); os.makedirs(basedir)
    ports = iter(range(18000+attempt*100, 18100+attempt*100))
    gspPort = next(ports)
    gspDir = basedir + '/gsp'; os.makedirs(gspDir)
    env = Environment(basedir, ports, '/usr/local/bin/xayax-eth')
    env.enablePending()
    with env.run() as e:
        e.generate(10)
        envV = dict(os.environ); envV['GLOG_log_dir'] = gspDir
        gsp_proc = subprocess.Popen([
            'build/rogueliked',
            '--xaya_rpc_url=' + e.getXRpcUrl(),
            '--xaya_rpc_protocol=2',
            '--game_rpc_port=%d' % gspPort,
            '--datadir=' + gspDir,
            '--genesis_height=0', '--genesis_hash=',
            '--xaya_rpc_wait', '--pending_moves',
            '--dungeon_id=repro_%d' % attempt,
        ], env=envV)
        time.sleep(2)
        gsp = jsonrpclib.ServerProxy('http://localhost:%d' % gspPort)
        gsp.getcurrentstate()

        e.register('p', 'hero')
        e.move('p', 'hero', json.dumps({'g': {GAME_ID: {'r': {}}}}))
        e.generate(1); time.sleep(0.3)
        e.move('p', 'hero', json.dumps({'g': {GAME_ID: {'d': {'depth': 1, 'dir': 'east'}}}}))
        e.generate(1); time.sleep(0.3)
        e.move('p', 'hero', json.dumps({'g': {GAME_ID: {'ec': {'id': 1}}}}))
        e.generate(1); time.sleep(0.3)

        info = gsp.getplayerinfo('hero')['data']
        vid = info['active_visit']['visit_id']
        seed = gsp.getsegmentinfo(1)['data']['seed']

        spec = importlib.util.spec_from_file_location('x', 'devnet/ai_explorer.py')
        mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
        survived, results, action_log = mod.playDungeon(seed, 1, info, useAi=False)

        e.move('p', 'hero', json.dumps({'g': {GAME_ID: {'xc': {
            'id': vid, 'results': results, 'actions': action_log
        }}}}))
        e.generate(1); time.sleep(0.5)
        info2 = gsp.getplayerinfo('hero')['data']
        status = 'FAIL' if info2['in_channel'] else 'OK'
        print('Attempt %d [%s]: survived=%s xp=%d kills=%d actions=%d seed=%s' % (
            attempt, status, survived, results['xp'], results['kills'],
            len(action_log), seed[:40]))
        gsp._notify.stop(); gsp_proc.wait(10)
    shutil.rmtree(basedir, True)
"
```

Typically 1-3 out of 10 attempts will show FAIL.

### Using the AI explorer (slower, full multi-segment test)

```bash
source ~/Explore/xayax/.venv/bin/activate
python3 devnet/ai_explorer.py hero 3
```

The explorer will attempt to discover and confirm 3 segments. Settlement
rejections show as `Settlement REJECTED (replay mismatch?)` in the output.

## What Has Been Ruled Out

1. **Player stats mismatch**: The `play.cpp` binary now accepts full stats
   as CLI args (level, str, dex, con, int, equipAtk, equipDef, potions).
   The explorer passes the exact on-chain values from `getplayerinfo`.

2. **Potion count mismatch**: Verified — the explorer reads the actual bag
   potion count and passes it. Fresh players have 3 health_potion.

3. **Action format mismatch**: The `"use"` action correctly maps
   `"item"` (not `"itemId"`). Move actions include `dx`/`dy`. All types
   (`move`, `wait`, `pickup`, `use`, `gate`) are handled.

4. **Simple runs work**: Runs with 0 kills (movement + gate exit only)
   always pass verification. The mismatch is specific to combat.

## Root Cause: FOUND

**The game engine is NOT the problem.** Step-by-step investigation ruled
out every game-logic hypothesis:

1. **DungeonGame is fully deterministic** — same actions produce identical
   results across 30 test runs with combat. Zero mismatches.

2. **The play binary never outputs error lines** — the autopilot never
   sends invalid actions. No line-sync issues.

3. **The explorer's action recording is correct** — recorded action logs
   replay identically through the play binary on a second pass.

4. **Player stats match exactly** — HP, max_hp, level, str/dex/con/int,
   equip_attack, equip_defense, and potions are identical between what
   the play binary uses and what the GSP would use.

**The actual bug: the EVM transaction carrying the `xc` move silently
reverts when the action array is large (~50+ actions, ~2-3KB JSON).**

Evidence: GSP logs show `Processing block N forward` with no
`Processing 1 moves...` for the block containing the `xc` submission.
The move was submitted (`env.move()` returned a txid) but the GSP never
received it from Xaya X. The smart contract `XayaAccounts.move()` call
likely reverts due to gas cost with large string payloads.

The `env.move()` in `xayax/eth.py` uses a fixed `gas: 500_000`. Long
action arrays encode as large calldata, which may exceed this limit.

## Immediate Fix (Applied)

Increased gas limit to 1.5M for move transactions with payloads > 2KB
in `devnet/ai_explorer.py` and `devnet/frontend_devnet.py`. Direct
contract call bypasses the xayax `env.move()` 500K limit.

## Long-term Strategy

See `docs/STRATEGY_action_proofs.md` for analysis of on-chain action
proof costs and alternative approaches for production.

## Files Involved

- `play.cpp` — interactive play binary (Create + action loop)
- `dungeongame.cpp` — `DungeonGame::Create`, `DungeonGame::Replay`,
  `ProcessAction`, `ProcessMonsterTurns`
- `moveprocessor.cpp:838` — `ProcessExitChannel` (GSP replay + comparison)
- `combat.cpp` — attack/defense calculations
- `monsters.cpp` — monster spawning, AI
- `devnet/ai_explorer.py` — multi-segment explorer that triggers the bug
