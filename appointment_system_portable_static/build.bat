@echo off
cd /d %~dp0
echo Cleaning old files...
del sqlite3.o 2>nul
del appointment_server.exe 2>nul

echo Checking compiler...
where gcc >nul 2>nul
if errorlevel 1 (
  echo ERROR: gcc was not found.
  echo Please install MSYS2 MinGW-w64/UCRT64 gcc, and add C:\msys64\ucrt64\bin to PATH.
  pause
  exit /b 1
)
where g++ >nul 2>nul
if errorlevel 1 (
  echo ERROR: g++ was not found.
  echo Please install MSYS2 MinGW-w64/UCRT64 g++, and add C:\msys64\ucrt64\bin to PATH.
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

echo Building portable appointment system...
echo This version uses static linking to avoid missing DLL errors on other PCs.
g++ -std=c++11 main.cpp sqlite3.o -I. -static -static-libgcc -static-libstdc++ -pthread -lws2_32 -o appointment_server.exe
if errorlevel 1 (
  echo ERROR: Static build failed. Trying fallback build...
  g++ -std=c++11 main.cpp sqlite3.o -I. -static -static-libgcc -static-libstdc++ -lws2_32 -o appointment_server.exe
  if errorlevel 1 (
    echo ERROR: C++ build failed.
    pause
    exit /b 1
  )
)

echo.
echo Build success: appointment_server.exe
echo This exe is more portable. Send the whole folder to classmates.
echo Double-click appointment_server.exe to start the system.
echo It should open http://127.0.0.1:8080 automatically.
echo.
pause
