@echo off
rem Starts the editor and waits for THIS session's MCP bridge, printing its startup lines.
rem
rem One script = one call. Three details that used to cause confusion:
rem   - the editor is launched with `start` in its OWN console (no /b), so this script does not stay
rem     attached to the caller's console and the caller's tool call returns;
rem   - the wait is delegated to wait_editor.bat (30s first, then every 5s), so nobody has to run a
rem     second wait by hand;
rem   - the log is APPENDED across sessions, so the current size is passed along: without it the wait
rem     matches a previous session's "Server started on" line and reports ready for an editor that is
rem     still loading.
setlocal

set LOG=%~dp0..\..\..\Saved\Logs\asset_test_UE55.log
set OFFSET=0
if exist "%LOG%" for %%A in ("%LOG%") do set OFFSET=%%~zA

rem The inner quotes matter: the project path contains a space and "(2)", so an unquoted path after
rem /c is split into "command + arguments" and the editor never launches.
start "UnrealMCP Editor" /min cmd /c ""%~dp0..\Editor.bat" start"
call "%~dp0wait_editor.bat" %OFFSET%
