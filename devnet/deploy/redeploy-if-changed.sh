#!/usr/bin/env bash
#
# Polls GitHub and redeploys the sandbox ONLY when code (not docs) has landed.
# Meant to run on a short systemd timer (rog-redeploy.timer). deploy.sh does
# the real work, this just decides whether to invoke it, so the frequent
# docs-sweep / devlog auto-commits don't wipe the sandbox world for a README
# tweak. A manual `deploy.sh` still works any time; the next poll then sees the
# repos already up to date and does nothing.
#
# "Code" = any changed path that is not markdown and not under docs/. That
# deliberately includes deploy.sh, the Dockerfile, the Caddyfile, schema.sql,
# etc. -- anything that changes what gets served or how.
#
# Runs as root (deploy.sh needs systemctl, /var/www, and docker).
set -euo pipefail

GSP_REPO="${GSP_REPO:-/opt/xayaroguelike}"
FRONTEND_REPO="${FRONTEND_REPO:-/opt/xaya-roguelike-frontend}"
DEPLOY="${DEPLOY:-$GSP_REPO/devnet/deploy/deploy.sh}"

log () { printf '%s redeploy: %s\n' "$(date -u +%H:%M:%S)" "$*"; }

# True (exit 0) if the commit range touches anything that isn't docs/markdown.
range_has_code () {
  local dir="$1" range="$2" f files
  files="$(git -C "$dir" diff --name-only "$range")" || return 0  # error -> be safe, deploy
  [ -z "$files" ] && return 1
  while IFS= read -r f; do
    [ -z "$f" ] && continue
    case "$f" in
      *.md|docs/*) : ;;   # docs -> ignore
      *)           return 0 ;;
    esac
  done <<< "$files"
  return 1
}

changed=0

# Fetch a repo; if origin advanced with code, flag a redeploy; if it advanced
# with docs only, fast-forward locally so we don't re-detect it next tick.
check_repo () {
  local dir="$1" branch local_sha remote_sha
  git -C "$dir" fetch --quiet origin
  branch="$(git -C "$dir" rev-parse --abbrev-ref HEAD)"
  local_sha="$(git -C "$dir" rev-parse HEAD)"
  remote_sha="$(git -C "$dir" rev-parse "origin/$branch")"

  [ "$local_sha" = "$remote_sha" ] && return 0   # already current
  if ! git -C "$dir" merge-base --is-ancestor "$local_sha" "$remote_sha"; then
    log "WARNING: $dir ($branch) is not behind origin -- diverged? skipping"
    return 0
  fi

  if range_has_code "$dir" "${local_sha}..${remote_sha}"; then
    log "code change in $dir ($branch)"
    changed=1                      # leave the pull to deploy.sh
  else
    git -C "$dir" merge --ff-only "origin/$branch" >/dev/null
    log "docs-only update fast-forwarded in $dir ($branch); no redeploy"
  fi
}

check_repo "$GSP_REPO"
[ -d "$FRONTEND_REPO/.git" ] && check_repo "$FRONTEND_REPO"

if [ "$changed" = "1" ]; then
  log "redeploying via $DEPLOY"
  exec "$DEPLOY"
fi
log "no code changes; nothing to do"
