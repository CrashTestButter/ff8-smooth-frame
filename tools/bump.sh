#!/bin/sh
set -e
cd "$(dirname "$0")/.."
F=mod/mod.xml
cur=$(sed -n 's|.*<Version>\([^<]*\)</Version>.*|\1|p' $F)
case $cur in
    *[!0-9.]* | *.*.* | '') echo "unexpected version '$cur' in $F" >&2; exit 1 ;;
esac
major=${cur%%.*}
frac=${cur#*.}
[ "$frac" = "$cur" ] && frac=0
minor=$(printf %s "$frac" | cut -c1)
bugfix=$(printf %s "$frac" | cut -c2)
[ -n "$(printf %s "$frac" | cut -c3-)" ] && { echo "unexpected version '$cur' in $F" >&2; exit 1; }
: "${minor:=0}" "${bugfix:=0}"

case $1 in
    major)  major=$((major + 1)); minor=0; bugfix=0 ;;
    minor)  [ "$minor" -lt 9 ] || { echo "minor is already 9, use major" >&2; exit 1; }
            minor=$((minor + 1)); bugfix=0 ;;
    bugfix) [ "$bugfix" -lt 9 ] || { echo "bugfix is already 9, use minor" >&2; exit 1; }
            bugfix=$((bugfix + 1)) ;;
    *)      echo "usage: tools/bump.sh major|minor|bugfix" >&2; exit 1 ;;
esac

new=$major.$minor
[ "$bugfix" -gt 0 ] && new=$new$bugfix
sed -i "s|<Version>$cur</Version>|<Version>$new</Version>|" $F
echo "$cur -> $new"
