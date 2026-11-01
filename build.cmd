@echo off
rem Configure and build with clang-cl + Ninja inside the Visual Studio build environment.
rem usage: build.cmd [Release|Debug]
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "delims=" %%i in ('call "%VSWHERE%" -latest -products * -property installationPath') do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set CLANG_CL=clang-cl
where clang-cl >nul 2>nul || set "CLANG_CL=%ProgramFiles%\LLVM\bin\clang-cl.exe"

rem configure every time (cheap) so the compiler choice for C and C++ always matches
cmake -S . -B build\clang-%CONFIG% -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% "-DCMAKE_C_COMPILER=%CLANG_CL%" "-DCMAKE_CXX_COMPILER=%CLANG_CL%" || exit /b 1
cmake --build build\clang-%CONFIG% || exit /b 1

rem The exe looks for game\ and user\ beside itself; in the build folder those are
rem junctions to this folder's NPUB30051\ (game data) and user\ folders.
if not exist "%~dp0user" mkdir "%~dp0user"
if not exist "build\clang-%CONFIG%\game" mklink /J "build\clang-%CONFIG%\game" "%~dp0NPUB30051" >nul || exit /b 1
if not exist "build\clang-%CONFIG%\user" mklink /J "build\clang-%CONFIG%\user" "%~dp0user" >nul || exit /b 1
rem live\ holds the optional Bomberman LIVE files (tools\media\make_live.py); linked even
rem while empty, so files made after this build are found
if not exist "%~dp0live" mkdir "%~dp0live"
if not exist "build\clang-%CONFIG%\live" mklink /J "build\clang-%CONFIG%\live" "%~dp0live" >nul || exit /b 1
