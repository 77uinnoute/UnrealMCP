@echo off
rem Waits until the editor's MCP bridge is listening on 127.0.0.1:55557 (i.e. the plugin loaded).
rem
rem Polls every 5s from the first second, so a fast editor start costs 5s and not a fixed minute.
rem The port is the readiness signal: the bridge only listens after the registry is sealed, while the
rem start line in Saved/Logs is appended across sessions and the log is rotated on startup, so a log
rem match can be a stale line from the previous session.
rem
rem Use: Editor.bat start   then   this script.

setlocal
set /a POLLS=0
set TEMPOUT=%TEMP%\unrealmcp_port_probe.txt

:wait
set /a POLLS+=1
if %POLLS% GTR 72 goto :timeout
powershell -NoProfile -Command "try { $c = New-Object Net.Sockets.TcpClient; $c.Connect('127.0.0.1',55557); 'PORT_OPEN'; $c.Close() } catch { }" >"%TEMPOUT%" 2>nul
findstr /C:"PORT_OPEN" "%TEMPOUT%" >nul 2>nul
if not errorlevel 1 goto :ready
ping -n 6 127.0.0.1 >nul
goto :wait

:ready
echo PORT_OPEN after %POLLS% poll(s)
exit /b 0

:timeout
echo PORT_CLOSED after %POLLS% polls - the editor is not listening on 127.0.0.1:55557 yet
exit /b 1
