#!/bin/bash
# Prepare a squashed public release commit from a tag of this (private) repo.
# usage: tools/release-public.sh v0.2
# Stages the tag's tree in ~/ff8/ff8-smooth-frame-public and writes a draft message; it does NOT
# commit or push - edit the message, then run the commands it prints.
set -euo pipefail
TAG="${1:?usage: release-public.sh <tag>}"
SRC="$(cd "$(dirname "$0")/.." && pwd)"
PUB="${PUB:-$HOME/ff8/ff8-smooth-frame-public}"
REMOTE="${REMOTE:-git@github.com:CrashTestButter/ff8-smooth-frame.git}"
git -C "$SRC" rev-parse -q --verify "refs/tags/$TAG" >/dev/null || { echo "no tag $TAG"; exit 1; }

if [ ! -d "$PUB/.git" ]; then
  mkdir -p "$PUB"; git -C "$PUB" init -q -b main; git -C "$PUB" remote add origin "$REMOTE"
  git -C "$PUB" fetch -q origin 2>/dev/null && git -C "$PUB" reset -q --soft origin/main 2>/dev/null || true
fi
# replace the working tree with the tag's tree (squash: only the result, no history)
git -C "$PUB" rm -rq --cached . 2>/dev/null || true
find "$PUB" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
git -C "$SRC" archive "$TAG" | tar -x -C "$PUB"
git -C "$PUB" add -A

PREV=$(git -C "$PUB" describe --tags --abbrev=0 2>/dev/null || true)
MSG="$PUB/.git/RELEASE_MSG"
{
  echo "FF8 Smooth Frames ${TAG#v}"
  echo
  git -C "$SRC" tag -l --format='%(contents)' "$TAG"
  echo
  echo "Changes since ${PREV:-the start} (private history, edit freely):"
  if [ -n "$PREV" ] && git -C "$SRC" rev-parse -q --verify "refs/tags/$PREV" >/dev/null; then
    git -C "$SRC" log --format='- %s' "$PREV..$TAG"
  else
    git -C "$SRC" log --format='- %s' "$TAG"
  fi
} > "$MSG"
git -C "$PUB" status --short | tail -n 5
echo "Staged $TAG in $PUB ($(git -C "$PUB" diff --cached --name-only | wc -l) files changed)."
echo "Draft message: $MSG"
echo "Review and publish:"
echo "  cd $PUB && git commit -e -F .git/RELEASE_MSG && git tag -a $TAG -m \"FF8 Smooth Frames ${TAG#v}\" && git push origin main --tags"
