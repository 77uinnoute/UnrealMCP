@echo off
setlocal

rem Engine root can be overridden: Build_UnrealMCP.bat [EnginePath] [ProjectPath]
rem
rem EnginePath  defaults to D:\UE_5.5
rem ProjectPath defaults to the *.uproject two levels above the plugin (the plugin lives in
rem              <Project>\Plugins\UnrealMCP). Upstream hard-coded its own host project here
rem              (test_5_7.uproject) together with an engine path that does not exist on this
rem              machine, and the script died with "系统找不到指定的路径"; discovering the project
rem              and deriving the target name from it keeps the script working in both trees.
set ENGINE_PATH=%~1
if "%ENGINE_PATH%"=="" set ENGINE_PATH=D:\UE_5.5

set PROJECT_PATH=%~2
if not "%PROJECT_PATH%"=="" goto :project_known
for %%F in ("%~dp0..\..\*.uproject") do set PROJECT_PATH=%%~fF
:project_known
if not exist "%PROJECT_PATH%" goto :project_missing
for %%F in ("%PROJECT_PATH%") do set TARGET_NAME=%%~nFEditor

set PLUGIN_PATH=%~dp0UnrealMCP.uplugin
set STATUS_FILE=%~dp0Build\build_status.txt
set RUNLOG=%~dp0Build\build_run.txt

echo ============================================
echo Building UnrealMCP plugin
echo Engine : %ENGINE_PATH%
echo Project: %PROJECT_PATH%
echo Target : %TARGET_NAME%
echo Plugin : %PLUGIN_PATH%
echo ============================================

if not exist "%~dp0Build" mkdir "%~dp0Build"

rem Clear the previous run's status before anything else: the status file is the "build finished"
rem signal (wait_status.bat polls for it from the first second), so leaving the old OK in place would
rem report the previous build's result as this one's.
if exist "%STATUS_FILE%" del /f /q "%STATUS_FILE%"
if exist "%RUNLOG%" del /f /q "%RUNLOG%"

echo.
echo [UnrealMCP] Closing Unreal Editor if running (binaries are locked while editor is open)...
taskkill /IM UnrealEditor.exe /F >nul 2>&1
if errorlevel 1 goto :editor_was_not_running
echo [UnrealMCP] Unreal Editor closed.
goto :editor_done
:editor_was_not_running
echo [UnrealMCP] Unreal Editor was not running.
:editor_done

rem Built as part of the PROJECT's editor target, not through RunUAT BuildPlugin.
rem
rem BuildPlugin compiles the plugin inside a throwaway host project under Intermediate/ and then
rem deletes it, so the deployed PDB points at object files that no longer exist. Live Coding patches
rem by relinking against exactly those object files, so the patch failed with
rem "LNK2011: precompiled object not linked in" and took the whole patch - project module included -
rem down with it.
rem
rem Building the project target leaves every intermediate of both modules in the project's
rem Intermediate/Build tree, which is what makes them patchable. It also needs no deploy step:
rem UBT writes Binaries/Win64/UnrealEditor-UnrealMCP.dll into the plugin folder directly.
rem
rem UBT output is buffered into build_run.txt (and reprinted below) so wait_status.bat can grep it
rem for compile errors - the same contract Build_Project.bat keeps.
echo.
echo [UnrealMCP] Building target %TARGET_NAME% (project + plugin, shared intermediates)...
call "%ENGINE_PATH%\Engine\Build\BatchFiles\Build.bat" %TARGET_NAME% Win64 Development -Project="%PROJECT_PATH%" -WaitMutex > "%RUNLOG%" 2>&1
if errorlevel 1 goto :build_failed

type "%RUNLOG%"
echo.
echo [UnrealMCP] Build SUCCEEDED.

rem The manifest MUST be present next to the plugin DLL: the editor compares its BuildId against the
rem engine's before loading the module, and a stale one (e.g. left behind by a build made with
rem another engine version) makes startup fail with "modules are missing or built with a different
rem engine version: UnrealMCP". UBT does not write one into the plugin folder, so seed it from the
rem project's, which describes the same engine build.
if exist "%~dp0..\..\Binaries\Win64\UnrealEditor.modules" (
    if not exist "%~dp0Binaries\Win64\UnrealEditor.modules" (
        copy /Y "%~dp0..\..\Binaries\Win64\UnrealEditor.modules" "%~dp0Binaries\Win64\UnrealEditor.modules"
        if errorlevel 1 goto :deploy_failed
    )
)
if not exist "%~dp0Binaries\Win64\UnrealEditor-UnrealMCP.dll" goto :deploy_failed
echo [UnrealMCP] Binaries in place. Restart the editor to load the new build.

echo.
echo [UnrealMCP] Checking the command surface (C++ registry vs python send sites vs tools vs skills)...
python "%~dp0Build\check_command_consistency.py" >> "%RUNLOG%" 2>&1
if errorlevel 1 goto :check_failed

rem The consistency check never imports python, so a tool module that raises at import time (a missing
rem typing name, a bad default) passes every check above and only surfaces when the MCP client fails
rem to connect. Import the server module the way the server does and count what came out.
echo.
echo [UnrealMCP] Checking that the python MCP server imports and registers its tools...
python "%~dp0Build\check_server_registration.py" >> "%RUNLOG%" 2>&1
if errorlevel 1 goto :registration_failed

findstr /I /C:"CONSISTENCY_RESULT" /C:"SERVER_REGISTRATION_RESULT" "%RUNLOG%" 2>nul
> "%STATUS_FILE%" echo OK %date% %time%
endlocal
exit /b 0

:project_missing
echo.
echo [UnrealMCP] No host project found at "%PROJECT_PATH%".
echo [UnrealMCP] Pass one explicitly: Build_UnrealMCP.bat [EnginePath] [ProjectPath]
> "%STATUS_FILE%" echo BUILD_FAILED %date% %time%
endlocal
exit /b 1

:build_failed
echo.
echo [UnrealMCP] Build FAILED. Last lines of the UBT log (%RUNLOG%):
powershell -NoProfile -Command "Get-Content -Tail 40 \"%RUNLOG%\"" 2>nul
> "%STATUS_FILE%" echo BUILD_FAILED %date% %time%
endlocal
exit /b 1

:deploy_failed
echo.
echo [UnrealMCP] Binaries missing after the build (expected "%~dp0Binaries\Win64\UnrealEditor-UnrealMCP.dll").
> "%STATUS_FILE%" echo DEPLOY_FAILED %date% %time%
endlocal
exit /b 2

:check_failed
echo.
echo [UnrealMCP] Command consistency check FAILED (see the drift report above).
powershell -NoProfile -Command "Get-Content -Tail 40 \"%RUNLOG%\"" 2>nul
echo [UnrealMCP] The binaries ARE built; fix the drift and run the check again.
> "%STATUS_FILE%" echo CHECK_FAILED %date% %time%
endlocal
exit /b 3

:registration_failed
echo.
echo [UnrealMCP] Python MCP server check FAILED: the server module cannot be imported or its tools
echo [UnrealMCP] do not register (see the traceback above). The MCP server will NOT start until this
echo [UnrealMCP] is fixed - the client only sees "Connection closed".
powershell -NoProfile -Command "Get-Content -Tail 40 \"%RUNLOG%\"" 2>nul
echo [UnrealMCP] The binaries ARE built; fix the python and run the check again.
> "%STATUS_FILE%" echo REGISTRATION_FAILED %date% %time%
endlocal
exit /b 4
