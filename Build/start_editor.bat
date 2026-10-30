@echo off
rem Starts the editor and waits for THIS session's MCP bridge, printing its startup lines.
rem
rem One script = one call. Four details that used to cause confusion:
rem   - the editor is launched with `start` in its OWN console (no /b), so this script does not stay
rem     attached to the caller's console and the caller's tool call returns;
rem   - the wait is delegated to wait_editor.bat (30s first, then every 5s), so nobody has to run a
rem     second wait by hand;
rem   - the log is APPENDED across sessions, so the current size is passed along: without it the wait
rem     matches a previous session's "Server started on" line and reports ready for an editor that is
rem     still loading.
rem   - it REFUSES to start a second editor: the port probe runs first, because a second instance binds
rem     127.0.0.1:55557 as well (observed 2026-10-13: two editors both LISTENING on the port, so MCP
rem     calls landed on either of them at random and saved assets from two divergent in-memory states).
rem     Pass "force" as the first argument to start another editor anyway.
setlocal

set PORT=55557
set FORCE=%~1
set PROBE=%TEMP%\unrealmcp_start_probe.txt

rem 0) is a bridge already listening? (same signal wait_bridge.bat uses)
powershell -NoProfile -Command "try { $c = New-Object Net.Sockets.TcpClient; $c.Connect('127.0.0.1',%PORT%); 'PORT_OPEN'; $c.Close() } catch { }" >"%PROBE%" 2>nul
findstr /C:"PORT_OPEN" "%PROBE%" >nul 2>nul
if not errorlevel 1 goto :already

rem 1) an editor running without a bridge? (the bridge only listens after the registry is sealed, so a
rem    still-loading or half-dead editor would otherwise get a second instance started on top of it)
tasklist /FI "IMAGENAME eq UnrealEditor.exe" /NH >"%TEMP%\unrealmcp_editors.txt" 2>nul
findstr /I /C:"UnrealEditor.exe" "%TEMP%\unrealmcp_editors.txt" >nul 2>nul
if not errorlevel 1 goto :no_bridge

if /I "%FORCE%"=="force" echo Starting another editor because "force" was passed.
goto :start

:already
echo ALREADY_RUNNING - the bridge is already listening on 127.0.0.1:%PORT%; not starting another editor.
echo   (pass "force" to start another one anyway)
exit /b 0

:no_bridge
if /I "%FORCE%"=="force" echo Starting another editor because "force" was passed.
if /I not "%FORCE%"=="force" goto :no_bridge_stop
goto :start

:no_bridge_stop
echo EDITOR_RUNNING_NO_BRIDGE - UnrealEditor.exe is running but 127.0.0.1:%PORT% is closed.
echo   Wait for it with Plugins\UnrealMCP\Build\wait_bridge.bat, or pass "force" to start another editor.
tasklist /FI "IMAGENAME eq UnrealEditor.exe" /FO TABLE
exit /b 2

:start
set LOG=%~dp0..\..\..\Saved\Logs\asset_test_UE55.log
set OFFSET=0
if exist "%LOG%" for %%A in ("%LOG%") do set OFFSET=%%~zA

rem The inner quotes matter: the project path contains a space and "(2)", so an unquoted path after
rem /c is split into "command + arguments" and the editor never launches.
start "UnrealMCP Editor" /min cmd /c ""%~dp0..\Editor.bat" start"
call "%~dp0wait_editor.bat" %OFFSET%
