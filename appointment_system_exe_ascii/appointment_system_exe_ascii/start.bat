@echo off
cd /d %~dp0
if not exist appointment_server.exe (
  echo appointment_server.exe was not found. Building first...
  call build.bat
)
if exist appointment_server.exe (
  echo Starting appointment system...
  appointment_server.exe
) else (
  echo Start failed because appointment_server.exe was not generated.
  pause
)
