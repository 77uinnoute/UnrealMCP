@echo off
REM Tile a numbered image sequence into one horizontal overview sheet (frame strip).
REM   usage: tile_images.bat <out.png> <dir-with-frames> <every-Nth-frame> <tiles> [prefix] [crop=W:H:X:Y]
REM Crop example from a PIE window recording: a 1936x1048 editor window whose viewport sits at 168,138
REM and is 1040x590 big  ->  crop=1040:590:168:138 .  Omitting the crop keeps the whole frame.
setlocal enabledelayedexpansion
set OUT=%~1
set DIR=%~2
set STEP=%~3
set TILES=%~4
set PRE=%~5
if "%PRE%"=="" set PRE=f
set CROP=%~6
if "%CROP%"=="" set CROP=crop=iw:ih:0:0
ffmpeg -y -loglevel error -i "%DIR%\%PRE%%%04d.jpg" -vf "%CROP%,select='not(mod(n\,%STEP%))',scale=-2:150,tile=%TILES%x1" -frames:v 1 "%OUT%"
echo wrote %OUT%
