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
The version comes from `<Version>` in `mod/mod.xml`; `-DVERSION=0.21` overrides it.
If CMake picks the wrong Visual Studio version, add `-G "Visual Studio 17 2022"` or
`-G "Visual Studio 18 2026"`.

## Checking a build

```sh
strings ff8interp.dll | grep -E 'ff8interp [0-9]\.|interpolation v[0-9]+ enabled|gf pacing v'
```

The first line must show the version from `mod/mod.xml`.

## Packaging

A plain CMake build is a development build: the log line reads `ff8interp 0.3-dev`, and
`ff8interp.toml` also accepts the kill switches and trace logging used for bisecting.

`tools/build-prod-release.sh <version>` makes the player build: kill switches and trace logging are
compiled out (`-DFF8INTERP_DEV=OFF`). It packs `dist/FF8SmoothFrames.iroj` and
`dist/FF8SmoothFrames.zip` with that version and today's date in `mod.xml`, plus one
`fpsNNN[_diag]/ff8interp.toml` per Configure Mod choice. It runs in Git Bash on Windows and needs
Python 3 (`PYTHON=python` if `python3` is not on the path).

`tools/pack_iro.py` writes the same layout as J8's `IrosArc.Create` with "no compression"
(`AppWrapper/IrosArc.cs`).

## Releasing

J8 reads `<Version>` as a decimal number, so the scheme is MAJOR.MINOR[BUGFIX] with one digit
each: 0.2, bugfix 0.21, minor 0.3, major 1.0. J8 refuses to import a mod with the same ID and the
same or a lower version.

```sh
tools/bump.sh bugfix            # or minor / major; edits mod/mod.xml
git commit -am "Version 0.21"
git tag -a v0.21                # the tag message becomes the release notes
git push origin main v0.21
```

The `release` workflow builds on a Windows runner and attaches `FF8SmoothFrames.iroj` and
`FF8SmoothFrames.zip` to the GitHub release. The file names carry no version, so
`releases/latest/download/FF8SmoothFrames.iroj` always points at the newest build. To redo a
release, delete the release and the remote tag, then push the tag again.
