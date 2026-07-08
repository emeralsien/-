@echo off
cd /d %~dp0
echo Cleaning old files...
del sqlite3.o 2>nul
del appointment_server.exe 2>nul

echo Checking compiler...
where gcc >nul 2>nul
if errorlevel 1 (
  echo ERROR: gcc was not found.
  echo Please install MinGW-w64 or MSYS2, and add gcc/g++ to PATH.
  echo Then open a new cmd window and run this file again.
  pause
  exit /b 1
)
where g++ >nul 2>nul
if errorlevel 1 (
  echo ERROR: g++ was not found.
  echo Please install MinGW-w64 or MSYS2, and add gcc/g++ to PATH.
  pause
  exit /b 1
)

echo Building SQLite...
gcc -c sqlite3.c -o sqlite3.o
if errorlevel 1 (
  echo ERROR: SQLite build failed.
  pause
  exit /b 1
)

echo Building appointment system...
g++ -std=c++11 main.cpp sqlite3.o -I. -lws2_32 -o appointment_server.exe
if errorlevel 1 (
  echo ERROR: C++ build failed.
  pause
  exit /b 1
)

echo.
echo Build success: appointment_server.exe
echo Double-click appointment_server.exe to start the system.
echo It should open http://127.0.0.1:8080 automatically.
echo.
pause
