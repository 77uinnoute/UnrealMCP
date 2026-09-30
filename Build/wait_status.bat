@echo off
rem Waits for a build to finish, then prints build_status.txt plus any compile errors.
rem
rem Use this instead of hand-writing a ping-based sleep: the interval lives HERE, so it cannot drift
rem from call to call. Run it right after starting Build_UnrealMCP.bat.
rem
rem Polls every 5s from the first second until the status file appears (max 6 min).
rem
rem It used to sleep a fixed 90s before the first check, which added 90s to EVERY build - even one
rem that finished in 20s - for no benefit: Build_UnrealMCP.bat clears the status file when it starts,
rem so the file existing means THIS build finished (not the previous one's leftover).
rem No parenthesised blocks on purpose: the project path contains "(2)", which cmd mis-parses inside
rem them once a path variable is expanded.

setlocal
set STATUS=%~dp0build_status.txt
set RUNLOG=%~dp0build_run.txt

set /a POLLS=0

:wait
if exist "%STATUS%" goto :report
set /a POLLS+=1
if %POLLS% GEQ 72 goto :timeout
if %POLLS% EQU 1 echo waiting for the build, polling every 5s ...
ping -n 6 127.0.0.1 >nul
goto :wait

:timeout
echo TIMEOUT - no build_status.txt after 72 polls (~6 min)
echo --- last lines of the build log ---
powershell -NoProfile -Command "Get-Content -Tail 15 \"%RUNLOG%\"" 2>nul
exit /b 1

:report
type "%STATUS%"
echo.
echo --- compile errors / check result ---
findstr /I /C:"error C" /C:"error LNK" /C:"Build FAILED" /C:"CONSISTENCY_RESULT" "%RUNLOG%" 2>nul
