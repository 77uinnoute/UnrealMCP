@echo off
rem Waits for this session's MCP bridge to come up, then prints its startup lines.
rem
rem Normally called by start_editor.bat, which passes the log size taken BEFORE the editor started:
rem the log is appended across sessions, so only lines written after that offset may count as "ready".
rem Without an offset (0) it looks at the whole log - fine for a first-ever run, wrong for a rerun.
rem
rem The offset is only valid while the log file keeps growing: when the editor starts it ROTATES the
rem log first (the previous run becomes asset_test_UE55-backup-<date>.log and a fresh, much smaller
rem asset_test_UE55.log is opened). A stale offset would then be larger than the new file, the read
rem below would never advance and the wait would always end in TIMEOUT. So the offset is dropped to 0
rem as soon as the file turns out to be smaller than the offset taken before the start.
rem
rem Waits 30s first, then polls every 5s (max ~5 min). No parenthesised blocks on purpose: the
rem project path contains "(2)", which cmd mis-parses inside them once a path variable is expanded.

setlocal
set LOG=%~dp0..\..\..\Saved\Logs\asset_test_UE55.log
set TAIL=%TEMP%\unreal_mcp_editor_tail.txt
set OFFSET=%~1
if "%OFFSET%"=="" set OFFSET=0
set READY="UnrealMCPBridge: Server started on"

ping -n 31 127.0.0.1 >nul

if not exist "%LOG%" goto :nolog

set /a POLLS=0

:wait
rem Copy the part of the log written after the offset so findstr only sees this session.
powershell -NoProfile -Command "$fi = New-Object IO.FileInfo('%LOG%'); $off = %OFFSET%; if ($fi.Length -lt $off) { $off = 0 }; if ($fi.Length -gt $off) { $fs = $fi.OpenRead(); $fs.Seek($off, 'Begin') | Out-Null; $sr = New-Object IO.StreamReader($fs); [IO.File]::WriteAllText('%TAIL%', $sr.ReadToEnd()); $sr.Close(); $fs.Close() } else { [IO.File]::WriteAllText('%TAIL%', '') }" 2>nul
findstr /C:%READY% "%TAIL%" >nul 2>&1
if not errorlevel 1 goto :report
set /a POLLS+=1
if %POLLS% GEQ 60 goto :timeout
if %POLLS% EQU 1 echo still starting, polling every 5s ...
ping -n 6 127.0.0.1 >nul
goto :wait

:timeout
echo TIMEOUT - the bridge did not log its startup line after 60 extra polls
exit /b 1

:nolog
echo PENDING - editor log not found at %LOG%
exit /b 1

:report
findstr /C:"MCPCommandRegistry: sealed" /C:%READY% "%TAIL%"
