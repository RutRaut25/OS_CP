@echo off
cd /d "%~dp0"

where gcc >nul 2>nul
if errorlevel 1 (
    echo GCC was not found. Install MinGW-w64 and add its bin folder to PATH.
    pause
    exit /b 1
)

echo Compiling ph2.c...
gcc -std=c11 -Wall -Wextra -O2 ph2.c -o ph2.exe
if errorlevel 1 (
    echo Compilation failed.
    pause
    exit /b 1
)

where node >nul 2>nul
if errorlevel 1 (
    echo Node.js was not found. Install Node.js and add it to PATH.
    pause
    exit /b 1
)

echo Starting the browser dashboard...
node server.js --open