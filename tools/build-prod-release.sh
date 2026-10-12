#!/bin/sh
# Usage: tools/build-prod-release.sh <version>
# Player build (kill bits and trace logging compiled out) packed as dist/FF8SmoothFrames.iroj and .zip.
# Runs in Git Bash on Windows with MSVC and CMake.
set -e
cd "$(dirname "$0")/.."
V=${1:?usage: tools/build-prod-release.sh <version>}
case $(uname -s) in
    MINGW* | MSYS* | CYGWIN*) ;;
    *) [ -x tools/winbuild.sh ] && exec tools/winbuild.sh "$V"
       echo "needs Git Bash on Windows with MSVC and CMake" >&2; exit 1 ;;
esac
PY=${PYTHON:-python3}
rm -rf build && mkdir -p build dist
cmake -S . -B build/cmake -A Win32 -DVERSION="$V" -DFF8INTERP_DEV=OFF
cmake --build build/cmake --config RelWithDebInfo
P=build/FF8SmoothFrames
mkdir $P
cp mod/mod.xml build/cmake/bin/RelWithDebInfo/ff8interp.dll $P/
# One ff8interp.toml per Configure Mod choice; J8 maps the active <ModFolder> over the game folder.
for d in $(sed -n 's|.*<ModFolder Folder="\(fps[0-9]*\(_diag\)\{0,1\}\)">.*|\1|p' mod/mod.xml); do
    fps=${d#fps}; fps=${fps%_diag}
    diag=false; [ "${d%_diag}" != "$d" ] && diag=true
    mkdir $P/$d
    sed "s|_fps = 144|_fps = $fps|; s|^ff8interp_diag = false|ff8interp_diag = $diag|" mod/ff8interp.toml > $P/$d/ff8interp.toml
    grep -q "ff8_field_fps = $fps" $P/$d/ff8interp.toml
done
[ -f $P/fps144/ff8interp.toml ]
sed -i "s|<Version>[^<]*</Version>|<Version>$V</Version>|; s|<ReleaseDate>[^<]*</ReleaseDate>|<ReleaseDate>$(date -u +%Y-%m-%d)</ReleaseDate>|" $P/mod.xml
grep -q "<Version>$V</Version>" $P/mod.xml
grep -aq "ff8interp $V (FF8 Smooth Frames)" $P/ff8interp.dll
! grep -aqE "ff8_battle_interp_debug|trace_battle_animation|queue B held" $P/ff8interp.dll
$PY tools/pack_iro.py $P dist/FF8SmoothFrames.iroj
rm -f dist/FF8SmoothFrames.zip
(cd build && $PY -m zipfile -c ../dist/FF8SmoothFrames.zip FF8SmoothFrames)
