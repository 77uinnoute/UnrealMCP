@echo off
setlocal

rem Engine root can be overridden: Build_UnrealMCP.bat [EnginePath] (e.g. F:\UnrealEngine-5.5 source build)
set ENGINE_PATH=%~1
if "%ENGINE_PATH%"=="" set ENGINE_PATH=D:\UE_5.5
set PLUGIN_PATH=%~dp0UnrealMCP.uplugin
set PACKAGE_PATH=%~dp0..\..\Intermediate\UnrealMCP_Packaged
set STATUS_FILE=%~dp0Build\build_status.txt

echo ============================================
echo Building UnrealMCP plugin (UE 5.5)
echo Engine : %ENGINE_PATH%
echo Plugin : %PLUGIN_PATH%
echo Package: %PACKAGE_PATH%
echo ============================================

if not exist "%~dp0Build" mkdir "%~dp0Build"

rem Clear the previous run's status before anything else: the status file is the "build finished"
rem signal (wait_status.bat polls for it from the first second), so leaving the old OK in place would
rem report the previous build's result as this one's.
if exist "%STATUS_FILE%" del /f /q "%STATUS_FILE%"

echo.
echo [UnrealMCP] Closing Unreal Editor if running (DLL is locked while editor is open)...
taskkill /IM UnrealEditor.exe /F >nul 2>&1
if %ERRORLEVEL%==0 (
    echo [UnrealMCP] Unreal Editor closed.
) else (
    echo [UnrealMCP] Unreal Editor was not running.
)

call "%ENGINE_PATH%\Engine\Build\BatchFiles\RunUAT.bat" BuildPlugin -Plugin="%PLUGIN_PATH%" -Package="%PACKAGE_PATH%" -Rocket
if errorlevel 1 goto :build_failed

echo.
echo [UnrealMCP] Build SUCCEEDED.
echo [UnrealMCP] Deploying binaries to %~dp0Binaries\Win64 ...
copy /Y "%PACKAGE_PATH%\Binaries\Win64\UnrealEditor-UnrealMCP.dll" "%~dp0Binaries\Win64\"
if errorlevel 1 goto :deploy_failed
rem The manifest MUST be deployed: the editor compares its BuildId against the engine's before loading
rem the module, and a stale one (e.g. left behind by a build made with another engine version) makes
rem startup fail with "modules are missing or built with a different engine version: UnrealMCP".
rem BuildPlugin names it UnrealEditor.modules (the packaged host target), not UnrealEditor-UnrealMCP.modules.
if exist "%PACKAGE_PATH%\Binaries\Win64\UnrealEditor.modules" (
    copy /Y "%PACKAGE_PATH%\Binaries\Win64\UnrealEditor.modules" "%~dp0Binaries\Win64\"
    if errorlevel 1 goto :deploy_failed
)
if exist "%PACKAGE_PATH%\Binaries\Win64\UnrealEditor-UnrealMCP.pdb" (
    copy /Y "%PACKAGE_PATH%\Binaries\Win64\UnrealEditor-UnrealMCP.pdb" "%~dp0Binaries\Win64\"
    if errorlevel 1 goto :deploy_failed
)
echo [UnrealMCP] Deploy done. Restart the editor to load the new build.

echo.
echo [UnrealMCP] Checking the command surface (C++ registry vs python send sites vs tools vs skills)...
python "%~dp0Build\check_command_consistency.py"
if errorlevel 1 goto :check_failed

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
echo [UnrealMCP] Deploy FAILED (copy error). Package binaries remain at "%PACKAGE_PATH%\Binaries\Win64".
> "%STATUS_FILE%" echo DEPLOY_FAILED %date% %time%
endlocal
exit /b 2

:check_failed
echo.
echo [UnrealMCP] Command consistency check FAILED (see the drift report above).
echo [UnrealMCP] The binaries ARE deployed; fix the drift and run the check again.
> "%STATUS_FILE%" echo CHECK_FAILED %date% %time%
endlocal
exit /b 3
