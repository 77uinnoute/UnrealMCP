@echo off
REM Fold a numbered image sequence (as written by ue_window_grab.ps1) into an mp4.
REM   usage: frames2video.bat <out.mp4> <fps> <dir-with-frames> [prefix]
REM Pass the ACHIEVED fps the grabber printed, not the fps you asked for.
setlocal enabledelayedexpansion
set OUT=%~1
set FPS=%~2
set DIR=%~3
set PRE=%~4
if "%PRE%"=="" set PRE=f
ffmpeg -y -loglevel error -framerate %FPS% -i "%DIR%\%PRE%%%04d.jpg" -c:v libx264 -crf 20 -pix_fmt yuv420p "%OUT%"
echo wrote %OUT%
