@echo off
setlocal

rem Engine root can be overridden: Build_UnrealMCP.bat [EnginePath]
set ENGINE_PATH=%~1
if "%ENGINE_PATH%"=="" set ENGINE_PATH=D:\ue\UE_5.7
set PROJECT_PATH=%~dp0..\..\test_5_7.uproject
set PLUGIN_PATH=%~dp0UnrealMCP.uplugin
set STATUS_FILE=%~dp0Build\build_status.txt

echo ============================================
echo Building UnrealMCP plugin (UE 5.7)
echo Engine : %ENGINE_PATH%
echo Project: %PROJECT_PATH%
echo Plugin : %PLUGIN_PATH%
echo ============================================

if not exist "%~dp0Build" mkdir "%~dp0Build"

rem Clear the previous run's status before anything else: the status file is the "build finished"
rem signal (wait_status.bat polls for it from the first second), so leaving the old OK in place would
rem report the previous build's result as this one's.
if exist "%STATUS_FILE%" del /f /q "%STATUS_FILE%"

echo.
echo [UnrealMCP] Closing Unreal Editor if running (binaries are locked while editor is open)...
taskkill /IM UnrealEditor.exe /F >nul 2>&1
if %ERRORLEVEL%==0 (
    echo [UnrealMCP] Unreal Editor closed.
) else (
    echo [UnrealMCP] Unreal Editor was not running.
)

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
echo.
echo [UnrealMCP] Building target test_5_7Editor (project + plugin, shared intermediates)...
call "%ENGINE_PATH%\Engine\Build\BatchFiles\Build.bat" test_5_7Editor Win64 Development -Project="%PROJECT_PATH%" -WaitMutex
if errorlevel 1 goto :build_failed

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
python "%~dp0Build\check_command_consistency.py"
if errorlevel 1 goto :check_failed

rem The consistency check never imports python, so a tool module that raises at import time (a missing
rem typing name, a bad default) passes every check above and only surfaces when the MCP client fails
rem to connect. Import the server module the way the server does and count what came out.
echo.
echo [UnrealMCP] Checking that the python MCP server imports and registers its tools...
python "%~dp0Build\check_server_registration.py"
if errorlevel 1 goto :registration_failed

> "%STATUS_FILE%" echo OK %date% %time%
endlocal
exit /b 0

:build_failed
echo.
echo [UnrealMCP] Build FAILED with error code %ERRORLEVEL%.
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
echo [UnrealMCP] The binaries ARE built; fix the drift and run the check again.
> "%STATUS_FILE%" echo CHECK_FAILED %date% %time%
endlocal
exit /b 3

:registration_failed
echo.
echo [UnrealMCP] Python MCP server check FAILED: the server module cannot be imported or its tools
echo [UnrealMCP] do not register (see the traceback above). The MCP server will NOT start until this
echo [UnrealMCP] is fixed - the client only sees "Connection closed".
echo [UnrealMCP] The binaries ARE built; fix the python and run the check again.
> "%STATUS_FILE%" echo REGISTRATION_FAILED %date% %time%
endlocal
exit /b 4
