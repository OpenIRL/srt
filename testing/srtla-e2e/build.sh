#!/bin/sh
# Build everything the SRTLA end-to-end harness needs, into ./build:
#   receiver-<name>  test receiver linked against this repo at <ref> (one per name=ref argument)
#   sender           test sender (own code) linked against the BELABOX libsrt fork (BELABOX/srt, MPL-2.0)
#   srtla_send       BELABOX's bonding sender (BELABOX/srtla, AGPL-3), compiled unmodified from its clone
#
# The two BELABOX repositories are cloned at pinned commits into build/deps/ and built as they are;
# nothing of them is copied into this repository.
#
# Usage: build.sh [name=ref ...]          default: head=HEAD
#   e.g. build.sh head=HEAD main=main
# Env:   BELABOX_SRT_REF, BELABOX_SRTLA_REF  commits of the BELABOX repositories (default: the pinned ones below)
#        CMAKE_EXTRA       extra cmake options for the receivers (e.g. -DENABLE_HEAVY_LOGGING=ON)
#        CMAKE                        cmake binary (default: cmake on PATH, else CLion's bundled one)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); REPO=$(cd "$HERE/../.." && pwd); B="$HERE/${BUILD_DIR:-build}"
# BELABOX sender stack, pinned to known-good commits; CI builds the same ones
BELABOX_SRT_REF=${BELABOX_SRT_REF:-a1054cfe8bf225d49356576137274396c5742b9d}
BELABOX_SRTLA_REF=${BELABOX_SRTLA_REF:-37862da3d0c13b46956efd3f88877053293d97d6}
if [ -z "${CMAKE:-}" ]; then
  if command -v cmake >/dev/null 2>&1; then CMAKE=cmake
  else CMAKE=$(ls /Applications/CLion.app/Contents/bin/cmake/mac/*/bin/cmake 2>/dev/null | head -1); fi
fi
[ -n "$CMAKE" ] || { echo "cmake not found (set CMAKE=...)"; exit 1; }
export CMAKE_POLICY_VERSION_MINIMUM=3.5   # both SRT trees declare cmake_minimum_required < 3.5; the env var also reaches sub-cmakes
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
CMAKE_FLAGS="-G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DENABLE_ENCRYPTION=OFF -DENABLE_APPS=OFF -DENABLE_UNITTESTS=OFF"
# CMAKE_EXTRA adds options for the receiver variants, e.g. CMAKE_EXTRA="-DENABLE_HEAVY_LOGGING=ON" for loss traces
CMAKE_FLAGS="$CMAKE_FLAGS ${CMAKE_EXTRA:-}"
command -v ninja >/dev/null 2>&1 || CMAKE_FLAGS=$(echo "$CMAKE_FLAGS" | sed 's/-G Ninja //')
mkdir -p "$B"

# fetch_pinned <url> <ref> <dir>: one commit of a BELABOX repository, shallow, detached; reused once it is there
fetch_pinned() {
  [ -d "$3/.git" ] || { git init -q "$3" && git -C "$3" remote add origin "$1"; }
  if git -C "$3" cat-file -e "$2^{commit}" 2>/dev/null; then git -C "$3" checkout -q --detach "$2"
  else git -C "$3" fetch -q --depth 1 origin "$2" && git -C "$3" checkout -q --detach FETCH_HEAD; fi
  echo "   $1 @ $(git -C "$3" log --oneline -1)"
}

# name=ref            receiver built from a git ref (checked out into build/wt/<name>)
# name=.              receiver built from the working tree as it is (uncommitted changes included)
# name=ref:FLAGS      same, with extra compiler flags, e.g. cap200=.:-DSRTLA_HOLD_CAP_MS=200
[ $# -gt 0 ] || set -- head=HEAD
for spec in "$@"; do
  name=${spec%%=*}; rest=${spec#*=}; ref=${rest%%:*}; flags=""
  case "$rest" in *:*) flags=${rest#*:};; esac
  if [ "$ref" = . ]; then
    wt="$REPO"; echo "== receiver-$name (working tree${flags:+, $flags})"
  else
    wt="$B/wt/$name"
    if [ -d "$wt" ]; then git -C "$wt" checkout -q --detach "$(git -C "$REPO" rev-parse "$ref")"
    else git -C "$REPO" worktree add -q -f --detach "$wt" "$ref"; fi
    echo "== receiver-$name ($(git -C "$wt" log --oneline -1)${flags:+, $flags})"
  fi
  # a build dir configured from another source tree (worktree vs working tree) has to start over
  if [ -f "$B/lib/$name/CMakeCache.txt" ] && ! grep -q "^CMAKE_HOME_DIRECTORY:INTERNAL=$wt\$" "$B/lib/$name/CMakeCache.txt"; then rm -rf "$B/lib/$name"; fi
  $CMAKE -S "$wt" -B "$B/lib/$name" $CMAKE_FLAGS -DCMAKE_CXX_FLAGS="$flags" >"$B/cmake-$name.log" 2>&1 || { tail -20 "$B/cmake-$name.log"; exit 1; }
  $CMAKE --build "$B/lib/$name" -j "$JOBS" >>"$B/cmake-$name.log" 2>&1 || { grep -E 'error' "$B/cmake-$name.log" | head; exit 1; }
  c++ -std=c++11 -O2 -g -I"$wt/srtcore" -I"$B/lib/$name" -o "$B/receiver-$name" "$HERE/receiver.cpp" "$B/lib/$name/libsrt.a" -lpthread
done

echo "== sender (BELABOX/srt)"
SRT="$B/deps/srt"; fetch_pinned https://github.com/BELABOX/srt.git "$BELABOX_SRT_REF" "$SRT"
$CMAKE -S "$SRT" -B "$B/lib/belabox-srt" $CMAKE_FLAGS >"$B/cmake-belabox-srt.log" 2>&1 || { tail -20 "$B/cmake-belabox-srt.log"; exit 1; }
$CMAKE --build "$B/lib/belabox-srt" -j "$JOBS" >>"$B/cmake-belabox-srt.log" 2>&1 || { grep -E 'error' "$B/cmake-belabox-srt.log" | head; exit 1; }
c++ -std=c++11 -O2 -g -I"$SRT/srtcore" -I"$B/lib/belabox-srt" -o "$B/sender" "$HERE/sender.cpp" "$B/lib/belabox-srt/libsrt.a" -lpthread

echo "== srtla_send (BELABOX/srtla)"
# Compiled unmodified, straight from the clone (AGPL-3, BELABOX). Nothing is copied or
# patched. On macOS a compatibility header supplies endian.h / CLOCK_MONOTONIC_COARSE /
# SOCK_NONBLOCK; on Linux the sources build as they are.
SRTLA="$B/deps/srtla"; fetch_pinned https://github.com/BELABOX/srtla.git "$BELABOX_SRTLA_REF" "$SRTLA"
SHIM=""
if [ "$(uname)" = Darwin ]; then
  mkdir -p "$B/shim" && cp "$HERE/macshim.h" "$B/shim/macshim.h" && printf '#pragma once\n#include "macshim.h"\n' >"$B/shim/endian.h"
  SHIM="-I$B/shim -include $B/shim/macshim.h"
fi
cc -O2 -g -w -DVERSION=\"e2e\" $SHIM -o "$B/srtla_send" "$SRTLA/srtla_send.c" "$SRTLA/common.c" || { echo "srtla_send build failed"; exit 1; }
echo "== done: $(ls "$B" | grep -E '^(receiver-|sender$|srtla_send$)' | tr '\n' ' ')"
