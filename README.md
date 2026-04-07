# Xaya Roguelike

A blockchain roguelike built on the [Xaya](https://xaya.io) game framework, targeting EVM chains (Polygon) via the Xaya X bridge. Players explore a persistent dungeon world where characters, inventory, and progression are stored on-chain, while real-time dungeon gameplay happens in off-chain game channels with cryptographic verification.

## How it works

- **On-chain overworld**: Players register, discover dungeon segments, travel between them, manage inventory, and equip items. The world map is a permanent graph of interconnected dungeon segments discovered by players.
- **Off-chain dungeon channels**: When a player enters a dungeon segment, a solo game channel opens. The dungeon session (movement, combat, items, monsters) runs locally in real-time. On exit, the player submits an action replay proof that the GSP verifies deterministically.
- **Deterministic generation**: Dungeon layouts, monster placement, and item spawns are fully deterministic from a seed. The same seed produces the exact same dungeon in both the C++ backend and the TypeScript frontend.

## Architecture

```
Polygon / Anvil (EVM)
        |
    Xaya X (bridge)
        |
    rogueliked (GSP)          -- on-chain game state processor
        |
   JSON-RPC API
    /       \
Browser      AI Player        -- clients
Frontend     (ai_player.py)
```

The **GSP** (Game State Processor) is the authoritative game logic. It reads moves from the blockchain, processes them, and maintains the SQLite game database. Clients connect via JSON-RPC to read state and submit moves.

## Project structure

```
CMakeLists.txt          Build system (FetchContent for deps)
main.cpp                GSP daemon entry point
logic.cpp/hpp           RoguelikeLogic (extends ChannelGame)
moveprocessor.cpp/hpp   Processes all 16 on-chain move types
moveparser.cpp/hpp      JSON move validation and parsing
statejson.cpp/hpp       State JSON extraction for RPC
rpcserver.cpp/hpp       Custom JSON-RPC methods
dungeon.cpp/hpp         Deterministic dungeon generation (80x40 grid)
dungeongame.cpp/hpp     Dungeon gameplay engine (combat, AI, items)
combat.cpp/hpp          Attack/defense/crit/dodge math
monsters.cpp/hpp        12 monster types scaled by depth
items.cpp/hpp           30 item definitions with real stats
pending.cpp/hpp         Pending move tracking
schema.sql              SQLite schema (players, segments, visits, etc.)
play.cpp                Standalone dungeon play binary (JSON stdin/stdout)
channelboard.cpp/hpp    Channel framework integration (BoardRules)
proto/                  Protobuf definitions for channel state
rpc-stubs/              JSON-RPC stub definitions
tests/                  Unit tests (132 tests)
devnet/                 Local development scripts
docs/                   Setup guide, security docs, segment lifecycle
```

## Building

### Prerequisites

See [docs/SETUP.md](docs/SETUP.md) for full system package list. Key dependencies:

- CMake 3.14+
- C++17 compiler
- libxayagame (fetched automatically via CMake FetchContent)
- SQLite3, protobuf, glog, jsoncpp, ZeroMQ, libmicrohttpd

### Compile

```bash
cmake -B build
cmake --build build -j$(nproc)
```

This produces:
- `build/rogueliked` -- the GSP daemon
- `build/roguelike-play` -- standalone dungeon play binary
- `build/roguelike-tests` -- unit test runner

### Run tests

```bash
cd build && ctest --output-on-failure
```

## Running

### Local devnet (for development)

The devnet scripts start a full local stack: Anvil (EVM node) + Xaya X (bridge) + rogueliked (GSP).

```bash
# Prerequisites: Foundry (anvil), Xaya X (xayax-eth), Python xayax package
source ~/Explore/xayax/.venv/bin/activate

# Run smoke tests (starts stack, runs 7 tests, tears down)
python3 devnet/smoke_test.py

# Run persistent devnet with move proxy for frontend development
python3 devnet/frontend_devnet.py
```

The frontend devnet prints connection info:
```
GSP RPC:     http://localhost:<port>
Move Proxy:  http://localhost:18380
```

### Production

```bash
./build/rogueliked \
  --xaya_rpc_url=<xaya-x-rpc-url> \
  --xaya_rpc_protocol=2 \
  --game_rpc_port=18332 \
  --datadir=/path/to/data \
  --genesis_height=<height> \
  --genesis_hash=<hash> \
  --pending_moves
```

## Game moves

All moves are JSON objects submitted as Xaya name updates under game ID `"rog"`:

| Move | Format | Description |
|------|--------|-------------|
| Register | `{"r": {}}` | Create a new player |
| Discover | `{"d": {"depth": N, "dir": "east"}}` | Discover a new segment |
| Travel | `{"t": {"dir": "east"}}` | Move to adjacent segment |
| Enter Channel | `{"ec": {"id": N}}` | Start a dungeon session |
| Exit Channel | `{"xc": {"id": N, "results": {...}, "actions": [...]}}` | Submit dungeon results with replay proof |
| Use Item | `{"ui": {"item": "health_potion"}}` | Use a consumable |
| Equip | `{"eq": {"rowid": N, "slot": "weapon"}}` | Equip an item |
| Unequip | `{"uq": {"rowid": N}}` | Unequip to bag |
| Allocate Stat | `{"as": {"stat": "strength"}}` | Spend a stat point |

## Frontend

The browser frontend lives in a separate repository: `~/Projects/xaya-roguelike-frontend/`

It connects to the GSP via JSON-RPC, displays the overworld segment map, and runs dungeon sessions locally with on-chain settlement.

## Security

- **Action replay verification**: Dungeon results are verified by replaying the full action sequence on-chain
- **Provisional segments**: New segments require discoverer to complete a channel run before becoming permanent
- **Discovery cooldown**: 50 blocks between discoveries to prevent world map spam
- **Deterministic RNG**: MT19937 seeded from SHA-256, identical across C++ and TypeScript

See [docs/SECURITY_Attack_and_Mitigations.md](docs/SECURITY_Attack_and_Mitigations.md) for detailed attack vector analysis.

## Documentation

- [PLAN.md](PLAN.md) -- Full development plan and phase status
- [docs/SETUP.md](docs/SETUP.md) -- Development environment setup
- [docs/SECURITY_Attack_and_Mitigations.md](docs/SECURITY_Attack_and_Mitigations.md) -- Attack vectors and mitigations
- [docs/segment-lifecycle.md](docs/segment-lifecycle.md) -- Segment discovery and lifecycle
