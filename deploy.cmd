@echo off
rem Copy a finished build into the clean play folder (default ..\bomberman), done when a
rem scope item / feature is complete. Copies bomberman.exe, the game files (NPUB30051 without
rem EBOOT.BIN / Thumbs.db) and live\. Never touches the play folder's user\ (the player's
rem settings, saves and trophies) and never removes files there.
rem usage: deploy.cmd [Release|Debug] [play folder]
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
set "DEST=%~2"
if "%DEST%"=="" set "DEST=%~dp0..\bomberman"
set "EXE=%~dp0build\clang-%CONFIG%\bomberman.exe"

if not exist "%EXE%" (echo No build at %EXE%; run build.cmd %CONFIG% first. & exit /b 1)
if not exist "%DEST%" mkdir "%DEST%"
copy /y "%EXE%" "%DEST%\" >nul || exit /b 1
rem robocopy: exit codes below 8 mean success
robocopy "%~dp0NPUB30051" "%DEST%\game" /E /XF Thumbs.db EBOOT.BIN /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1
if exist "%~dp0live" (
	robocopy "%~dp0live" "%DEST%\live" /E /NFL /NDL /NJH /NJS /NP >nul
	if errorlevel 8 exit /b 1
)
echo Deployed %CONFIG% build to %DEST%
exit /b 0
