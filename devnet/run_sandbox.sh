#!/usr/bin/env bash
#
# Launches the full sandbox stack (anvil + xayax-eth + rogueliked + move
# proxy) for a hosted deployment. Intended to be run under a process
# supervisor (systemd) so it restarts on crash. Restarting starts a fresh
# chain (the sandbox world resets), which is the intended periodic-reset
# behaviour for the public demo.
#
# Override paths via environment if your layout differs:
#   ROG_VENV     path to the xayax Python venv (default ~/Explore/xayax/.venv)
#   ROG_PROJECT  path to this repo (default: parent of this script)
set -euo pipefail

ROG_PROJECT="${ROG_PROJECT:-$(cd "$(dirname "$0")/.." && pwd)}"
ROG_VENV="${ROG_VENV:-$HOME/Explore/xayax/.venv}"

# Foundry (anvil) must be on PATH; frontend_devnet.py also prepends it.
export PATH="$HOME/.foundry/bin:$PATH"

# shellcheck disable=SC1091
source "$ROG_VENV/bin/activate"

exec python3 "$ROG_PROJECT/devnet/frontend_devnet.py"
