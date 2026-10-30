@echo off
setlocal

set EDITOR_EXE=D:\UE_5.5\Engine\Binaries\Win64\UnrealEditor.exe
set PROJECT=%~dp0..\..\asset_test_UE55.uproject

if "%1"=="start" (
    rem The editor's main window state comes from [RootWindow] in the PROJECT ini
    rem (Saved\Config\WindowsEditor\EditorPerProjectUserSettings.ini). The engine default is
    rem "maximized", but a single clean exit of a windowed (1280x720) session persists
    rem InitiallyMaximized=False, and our taskkill-based restarts never rewrite it - so the
    rem editor then opens non-maximized forever. Force the key before every launch.
    rem Ref: Engine\Source\Editor\MainFrame\Private\Frame\RootWindowLocation.h:38
    powershell -NoProfile -Command "$f='%~dp0..\..\Saved\Config\WindowsEditor\EditorPerProjectUserSettings.ini'; if (Test-Path $f) { $t=Get-Content -Raw $f; if ($t -match '(?m)^InitiallyMaximized=') { $t=$t -replace '(?m)^InitiallyMaximized=\w+','InitiallyMaximized=True' } elseif ($t -match '(?m)^\[RootWindow\]') { $t=$t -replace '(?m)^\[RootWindow\]',\"[RootWindow]`r`nInitiallyMaximized=True\" } else { $t=$t+\"`r`n[RootWindow]`r`nInitiallyMaximized=True`r`n\" }; Set-Content -NoNewline -Path $f -Value $t }"
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
