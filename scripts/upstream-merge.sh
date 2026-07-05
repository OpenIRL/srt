#!/usr/bin/env bash
#
# upstream-merge.sh — merge a Haivision SRT release into `main`.
#
# Run this after the "Upstream: merge Haivision SRT <tag>" issue is opened by the
# upstream-release-watch workflow. It:
#   1. points the `upstream` mirror branch at the release commit and pushes it
#      (your local git credentials may push the workflow files GITHUB_TOKEN can't),
#   2. merges `upstream` into `main`.
# On a clean merge it commits locally and stops (you review + push). On conflicts
# it stops with the file list — the SRTLA extension is woven into core libsrt
# files, so conflicts there are expected. It never pushes `main` for you.
#
# Usage:
#   scripts/upstream-merge.sh [TAG]
#     TAG  Haivision release tag (e.g. v1.5.5). Default: latest stable release
#          (needs `gh`). The tracking issue passes the tag explicitly.

set -euo pipefail

HAIVISION_URL="https://github.com/Haivision/srt.git"
MAIN_BRANCH="main"
MIRROR_BRANCH="upstream"

info() { printf '\033[1;34m▶ %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m! %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31m✘ %s\033[0m\n' "$*" >&2; exit 1; }

# --- preconditions -----------------------------------------------------------
command -v git >/dev/null 2>&1 || die "git not found"
git rev-parse --git-dir >/dev/null 2>&1 || die "not inside a git repository"
[ -z "$(git status --porcelain)" ] || die "working tree is not clean — commit or stash your changes first."

# --- ensure the Haivision remote exists --------------------------------------
if ! git remote get-url haivision >/dev/null 2>&1; then
  info "adding 'haivision' remote ($HAIVISION_URL)"
  git remote add haivision "$HAIVISION_URL"
fi

# --- determine the release tag -----------------------------------------------
TAG="${1:-}"
if [ -z "$TAG" ]; then
  command -v gh >/dev/null 2>&1 || die "no TAG argument and 'gh' is not available to resolve the latest release"
  info "resolving latest stable Haivision release"
  TAG="$(gh api repos/Haivision/srt/releases/latest --jq .tag_name)"
fi
info "target release: $TAG"

# --- fetch the release commit ------------------------------------------------
info "fetching $TAG from Haivision"
git fetch --no-tags haivision "+refs/tags/$TAG:refs/tags/$TAG"
SHA="$(git rev-parse "refs/tags/$TAG^{commit}")"
info "release commit: $SHA"

# --- update main first, so we can safely re-point the mirror branch ----------
info "checking out $MAIN_BRANCH and fast-forwarding from origin"
git checkout "$MAIN_BRANCH"
git pull --ff-only origin "$MAIN_BRANCH"

if git merge-base --is-ancestor "$SHA" HEAD; then
  info "$MAIN_BRANCH already contains $TAG — nothing to merge. ✅"
  exit 0
fi

# --- point the mirror branch at the release and publish it -------------------
info "pointing '$MIRROR_BRANCH' at $TAG"
git branch -f "$MIRROR_BRANCH" "$SHA"
if git push --force-with-lease origin "$MIRROR_BRANCH"; then
  info "pushed '$MIRROR_BRANCH' to origin"
else
  warn "could not push '$MIRROR_BRANCH' to origin — continuing with the local merge anyway."
fi

# --- merge -------------------------------------------------------------------
info "merging '$MIRROR_BRANCH' ($TAG) into $MAIN_BRANCH"
if git merge --no-ff --no-edit -m "Merge Haivision SRT $TAG into $MAIN_BRANCH" "$MIRROR_BRANCH"; then
  cat <<EOF

✅ Clean merge of $TAG into $MAIN_BRANCH (committed locally, NOT pushed).

Next:
  • build & test the SRTLA integration
  • git push origin $MAIN_BRANCH
  • close the tracking issue
EOF
else
  warn "merge hit conflicts (expected around the SRTLA-touched files)."
  echo
  echo "Conflicted files:"
  git diff --name-only --diff-filter=U | sed 's/^/  • /'
  cat <<EOF

Resolve them, keeping the SRTLA integration intact, then:
  git add -A
  git commit            # keep the merge commit — do NOT squash
  # build & test, then:
  git push origin $MAIN_BRANCH

To abort and return to a clean $MAIN_BRANCH:
  git merge --abort
EOF
  exit 1
fi
