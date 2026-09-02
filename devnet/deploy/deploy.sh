#!/usr/bin/env bash
#
# Redeploys the hosted sandbox, on the box, pulling everything from GitHub.
#
# Nothing is copied up from a workstation: the GSP is built from a pinned
# commit inside the image, and the frontend is built here from its own repo.
# So a deploy depends only on what is on GitHub, not on the state of whoever
# happens to be running it.
#
#   sudo /opt/xayaroguelike/devnet/deploy/deploy.sh              # main of both repos
#   sudo /opt/xayaroguelike/devnet/deploy/deploy.sh 398f144      # a specific GSP commit
#
# What it does, in order:
#   1. pull both repos
#   2. build the GSP image at a pinned commit (bypassing the layer-cache trap)
#   3. prove that image works, before anything live points at it
#   4. restart the sandbox service (this RESETS the world -- see below)
#   5. build the frontend and swap it in with one atomic rename
#
# The chain lives in anvil's memory, so step 4 always starts a fresh world.
# Every player loses their progress on every deploy.  That is inherent to the
# sandbox, not something this script chose.
set -euo pipefail

GSP_REPO="${GSP_REPO:-/opt/xayaroguelike}"
FRONTEND_REPO="${FRONTEND_REPO:-/opt/xaya-roguelike-frontend}"
FRONTEND_URL="${FRONTEND_URL:-https://github.com/EdwardAThomson/xaya-roguelike-frontend.git}"
WEB_ROOT="${WEB_ROOT:-/var/www/rog-frontend}"
SERVICE="${SERVICE:-rog-sandbox-docker}"
NODE_IMAGE="${NODE_IMAGE:-node:22-slim}"

say () { printf '\n=== %s\n' "$*"; }

# ---------------------------------------------------------------- 1. sources
say "Updating sources"
git -C "$GSP_REPO" pull --ff-only
if [ ! -d "$FRONTEND_REPO/.git" ]; then
  git clone "$FRONTEND_URL" "$FRONTEND_REPO"
else
  git -C "$FRONTEND_REPO" pull --ff-only
fi

# The commit to build.  Default to whatever the GSP repo is now on, so the
# image matches the checkout that is driving this deploy.
ROG_COMMIT="${1:-$(git -C "$GSP_REPO" rev-parse HEAD)}"
FRONTEND_COMMIT="$(git -C "$FRONTEND_REPO" rev-parse --short HEAD)"
say "GSP commit ${ROG_COMMIT} / frontend ${FRONTEND_COMMIT}"

# ---------------------------------------------------------------- 2. image
# --build-arg ROG_COMMIT is what makes the clone layer rebuild; without it
# Docker reuses the cached clone and ships an old GSP without complaining.
say "Building GSP image"
docker build --build-arg "ROG_COMMIT=${ROG_COMMIT}" \
  -t rog-sandbox "$GSP_REPO/devnet/deploy"

# ---------------------------------------------------------------- 3. prove it
# Run the full stack end to end inside the fresh image BEFORE the live service
# is pointed at it: register, discover, play a winning run, confirm a segment.
# A broken build stops here, with the old one still serving.
say "Smoke-testing the new image"
docker run --rm rog-sandbox \
  /opt/xayax/.venv/bin/python3 devnet/smoke_test.py

# ---------------------------------------------------------------- 4. backend
say "Restarting ${SERVICE} (the world resets here)"
systemctl restart "$SERVICE"

# ---------------------------------------------------------------- 5. frontend
# Built in a container so the box needs no Node toolchain of its own.
# PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD matters: the e2e harness is a devDependency
# and tsc lives in devDependencies too, so we cannot --omit=dev, but we have no
# use for ~400MB of browsers on a server.  node_modules ends up root-owned,
# which is fine since this script already needs root for systemctl.
say "Building frontend"
docker run --rm -e PLAYWRIGHT_SKIP_BROWSER_DOWNLOAD=1 \
  -v "$FRONTEND_REPO:/app" -w /app "$NODE_IMAGE" \
  sh -c 'npm ci --no-audit --no-fund && npx tsc'

# Stage the build beside the live one, then swap with a single rename.  `ln -T`
# plus `mv -T` replaces the symlink atomically: there is no instant where
# `current` is missing or points at a half-copied directory.
BUILD_DIR="$WEB_ROOT/builds/$FRONTEND_COMMIT"
say "Publishing to ${BUILD_DIR}"
mkdir -p "$BUILD_DIR"
cp -a "$FRONTEND_REPO/index.html" "$FRONTEND_REPO/style.css" "$BUILD_DIR/"
rm -rf "$BUILD_DIR/dist" "$BUILD_DIR/assets"
cp -a "$FRONTEND_REPO/dist" "$BUILD_DIR/"
if [ -d "$FRONTEND_REPO/assets" ]; then
  cp -a "$FRONTEND_REPO/assets" "$BUILD_DIR/"
fi

ln -sfnT "$BUILD_DIR" "$WEB_ROOT/current.new"
mv -T "$WEB_ROOT/current.new" "$WEB_ROOT/current"

# Keep the last few builds so a rollback is one symlink flip:
#   ln -sfnT /var/www/rog-frontend/builds/<sha> /var/www/rog-frontend/current.new
#   mv -T /var/www/rog-frontend/current.new /var/www/rog-frontend/current
(ls -1dt "$WEB_ROOT"/builds/*/ 2>/dev/null || true) | tail -n +6 | xargs -r rm -rf

# ---------------------------------------------------------------- verify
say "Deployed"
systemctl is-active "$SERVICE" | sed 's/^/service: /'
printf 'serving: %s\n' "$(readlink -f "$WEB_ROOT/current")"
printf '\nCheck it:\n'
printf '  curl -s https://xayarogue.octonion.io/gsp -H "content-type: application/json" \\\n'
printf "    -d '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getnullstate\",\"params\":[]}'\n"
printf '  # and open the site: register, walk through a gate, complete a run\n'
