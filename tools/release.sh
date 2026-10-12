#!/bin/sh
# Usage: tools/release.sh <version>   (writes dist/FF8SmoothFrames.iroj and .zip; needs MSVC and CMake)
set -e
cd "$(dirname "$0")/.."
V=${1:?usage: tools/release.sh <version>}
PY=${PYTHON:-python3}
rm -rf build && mkdir -p build dist
cmake -S . -B build/cmake -A Win32 -DVERSION="$V"
cmake --build build/cmake --config RelWithDebInfo
P=build/FF8SmoothFrames
mkdir $P
cp mod/mod.xml mod/ff8interp.toml build/cmake/bin/RelWithDebInfo/ff8interp.dll $P/
sed -i "s|<Version>[^<]*</Version>|<Version>$V</Version>|; s|<ReleaseDate>[^<]*</ReleaseDate>|<ReleaseDate>$(date -u +%Y-%m-%d)</ReleaseDate>|" $P/mod.xml
grep -q "<Version>$V</Version>" $P/mod.xml
grep -aq "ff8interp $V (FF8 Smooth Frames)" $P/ff8interp.dll
$PY tools/pack_iro.py $P dist/FF8SmoothFrames.iroj
rm -f dist/FF8SmoothFrames.zip
(cd build && $PY -m zipfile -c ../dist/FF8SmoothFrames.zip FF8SmoothFrames)
