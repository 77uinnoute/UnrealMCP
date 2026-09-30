@echo off
setlocal

rem Builds the PROJECT module (Source\asset_test_UE55) for the editor target.
rem Engine root can be overridden: Build_Project.bat [EnginePath]  (e.g. F:\UnrealEngine-5.5)
rem
rem Contract kept identical to Build_UnrealMCP.bat so the same waiter works:
rem   - clears Build\build_status.txt first, writes OK / BUILD_FAILED at the end
rem   - kills the editor first (the module DLL is locked while it is open)
rem   - UBT output goes to Build\build_run.txt, which wait_status.bat greps for compile errors
rem No parenthesised blocks on purpose: the project path contains "(2)", which cmd mis-parses
rem inside them once a path variable is expanded.

set ENGINE_PATH=%~1
if "%ENGINE_PATH%"=="" set ENGINE_PATH=D:\UE_5.5
set PROJECT_PATH=%~dp0..\..\asset_test_UE55.uproject
set STATUS_FILE=%~dp0Build\build_status.txt
set RUNLOG=%~dp0Build\build_run.txt

echo ============================================
echo Building project module asset_test_UE55 (UE 5.5)
echo Engine : %ENGINE_PATH%
echo Project: %PROJECT_PATH%
echo ============================================

if not exist "%~dp0Build" mkdir "%~dp0Build"

rem Clear the previous run's status before anything else: wait_status.bat treats the file's presence
rem as "this build finished", so a leftover OK would be reported as this run's result.
if exist "%STATUS_FILE%" del /f /q "%STATUS_FILE%"
if exist "%RUNLOG%" del /f /q "%RUNLOG%"

echo.
echo [Project] Closing Unreal Editor if running (module DLL is locked while editor is open)...
taskkill /IM UnrealEditor.exe /F >nul 2>&1
if errorlevel 1 goto :editor_was_not_running
echo [Project] Unreal Editor closed.
goto :editor_done
:editor_was_not_running
echo [Project] Unreal Editor was not running.
:editor_done

echo.
echo [Project] Calling UBT (output is buffered; it is printed again after the build)...
call "%ENGINE_PATH%\Engine\Build\BatchFiles\Build.bat" asset_test_UE55Editor Win64 Development -project="%PROJECT_PATH%" -waitmutex > "%RUNLOG%" 2>&1
if errorlevel 1 goto :build_failed

echo.
type "%RUNLOG%"
echo.
echo [Project] Build SUCCEEDED. Restart the editor to load the new module.
> "%STATUS_FILE%" echo OK %date% %time%
endlocal
exit /b 0

:build_failed
echo.
echo [Project] Build FAILED. Last lines of the UBT log:
powershell -NoProfile -Command "Get-Content -Tail 30 \"%RUNLOG%\"" 2>nul
> "%STATUS_FILE%" echo BUILD_FAILED %date% %time%
endlocal
exit /b 1
