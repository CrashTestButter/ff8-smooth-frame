# Building ff8interp.dll

The repo builds on its own; there are no dependencies beyond MSVC and the Windows SDK. It has
to be a 32-bit (Win32/x86) DLL, because FF8 is a 32-bit game. The build links the CRT statically
(`/MT`), so players need no VC++ redistributable. The finished DLL imports only KERNEL32 and
VERSION.

## Any Windows machine

Requirements: Visual Studio 2022 or 2026 (or its Build Tools) with "Desktop development with
C++", plus CMake 3.21 or newer. The CMake that comes with VS is fine.

```bat
cmake -S . -B .build -A Win32
cmake --build .build --config RelWithDebInfo
```

This produces `.build\bin\RelWithDebInfo\ff8interp.dll` and its `.pdb`. Copy the DLL into `mod\`.
If CMake picks the wrong Visual Studio version, add `-G "Visual Studio 17 2022"` or
`-G "Visual Studio 18 2026"`.

## Checking a build

```sh
strings ff8interp.dll | grep -E 'ff8interp 0\.|interpolation v[0-9]+ enabled|gf pacing v'
```

For 0.2 the output must list `ff8interp 0.2`, battle v74, world map v22, field v5 and gf pacing v4.
Bump the version in these places together:
- `src/standalone.h` (`FF8INTERP_VERSION`)
- `src/version.rc`
- `CMakeLists.txt` (`project(... VERSION)`)
- `mod/mod.xml` (`<Version>`)

## Maintainer setup (Linux host + Windows VM)

This is how the release is built: a dockur/windows VM (`~/ff8/winbuild`) with VS 2026 Build Tools.
The host folder `~/ff8` appears in the VM as `\\host.lan\Data`.

```sh
cd ~/ff8/winbuild && docker compose start
ssh -p 2222 todi@127.0.0.1 "cmd /c \\\\host.lan\\Data\\ff8interp\\build.cmd > \\\\host.lan\\Data\\ff8interp\\build.log 2>&1"
tail ~/ff8/ff8interp/build.log      # ends with "== done"
cd ~/ff8/winbuild && docker compose stop
```

`build.cmd` does the following:
1. mirrors the repo (`%SRC%`, default `\\host.lan\Data\ff8interp`) to `C:\src\ff8i\ff8interp`, because MSBuild does not like UNC trees;
2. touches every file, because the VM clock drifts;
3. configures `-G "Visual Studio 18 2026" -A Win32` and does a clean RelWithDebInfo build;
4. copies `ff8interp.dll` and `.pdb` to `%SRC%\out\`.

The whole run takes about a minute. Never run two builds at once: both write the same PDB.

## Packaging

```sh
cp out/ff8interp.dll mod/
mkdir -p dist
python3 tools/pack_iro.py mod dist/FF8SmoothFrames-0.2.iro       # J8 archive, stored, self-verified
python3 -c "import zipfile; z=zipfile.ZipFile('dist/FF8SmoothFrames-0.2.zip','w',zipfile.ZIP_DEFLATED); [z.write('mod/'+f,'FF8SmoothFrames/'+f) for f in ('mod.xml','ff8interp.dll','ff8interp.toml')]; z.close()"
```

`tools/pack_iro.py` writes the same layout as J8's `IrosArc.Create` with "no compression"
(`AppWrapper/IrosArc.cs`). To use J8's own packer instead: **Tools → IRO Tools...** in Junction
VIII, source folder = `mod/`.
