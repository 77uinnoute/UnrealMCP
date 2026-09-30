@echo off
setlocal

set EDITOR_EXE=D:\UE_5.5\Engine\Binaries\Win64\UnrealEditor.exe
set PROJECT=%~dp0..\..\asset_test_UE55.uproject

if "%1"=="start" (
    rem -AutoDeclinePackageRecovery: after taskkill the editor would pop the
    rem "restore unsaved packages" modal on startup, blocking the GameThread
    rem (no viewport, MCP screenshots fail). This flag acts as if the user
    rem declined recovery and cleans up the restore files. Cost: autosave
    rem recovery is discarded.
    start "UnrealEditor" "%EDITOR_EXE%" "%PROJECT%" -AutoDeclinePackageRecovery
    echo [UnrealMCP] Editor starting...
) else if "%1"=="stop" (
    taskkill /IM UnrealEditor.exe /F >nul 2>&1
    echo [UnrealMCP] Editor stopped.
) else (
    echo Usage: %~nx0 [start^|stop]
)

endlocal
