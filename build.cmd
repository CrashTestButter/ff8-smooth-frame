@echo off
rem Build ff8interp.dll inside the Windows VM. Run:
rem   ssh -p 2222 todi@127.0.0.1 "cmd /c \\host.lan\Data\ff8interp\build.cmd > \\host.lan\Data\ff8interp\build.log 2>&1"
rem Sources: \\host.lan\Data\ff8interp (this repo, incl. interp/), mirrored to C:\src\ff8i\ff8interp because
rem MSBuild does not like UNC trees. Usable from any checkout: set SRC to the repo path.
rem Output: %SRC%\out\ff8interp.dll (+ .pdb)
setlocal
set ROOT=C:\src\ff8i
if "%SRC%"=="" set SRC=\\host.lan\Data\ff8interp
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=RelWithDebInfo

echo == sync sources
robocopy %SRC% %ROOT%\ff8interp /MIR /IS /IT /NFL /NDL /NJH /NJS /XD out .build .git dist /XF build.log *.iro *.zip
rem VM clock drift: touch everything so MSBuild recompiles what changed
forfiles /p %ROOT% /s /m *.* /c "cmd /c copy /b @path+,, @path >nul" 2>nul

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -products * -prerelease -latest -property installationPath`) do set VSPATH=%%i
echo == VS at %VSPATH%
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1

cd /d %ROOT%\ff8interp
echo == configure
cmake -S . -B .build -G "Visual Studio 18 2026" -A Win32 || exit /b 1
echo == build %CONFIG%
cmake --build .build --config %CONFIG% --clean-first -- /v:minimal || exit /b 1

echo == collect
if not exist %SRC%\out mkdir %SRC%\out
copy /y .build\bin\%CONFIG%\ff8interp.dll %SRC%\out\ || copy /y .build\bin\ff8interp.dll %SRC%\out\ || exit /b 1
copy /y .build\bin\%CONFIG%\ff8interp.pdb %SRC%\out\ 2>nul || copy /y .build\bin\ff8interp.pdb %SRC%\out\ 2>nul
echo == done
endlocal
