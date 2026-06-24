@echo off
REM ============================================================
REM  Build Look20 with MinGW (gcc + windres).
REM  Produces a single self-contained Look20.exe (no DLLs, no
REM  external assets). Just double-click this file, or run it
REM  from a terminal.
REM ============================================================

setlocal

REM --- locate the MinGW toolchain -----------------------------
set "MINGW=C:\msys64\mingw64\bin"
set "GCC=%MINGW%\gcc.exe"
set "WINDRES=%MINGW%\windres.exe"

if not exist "%GCC%" (
    echo [!] gcc not found at %GCC%
    echo     Edit the MINGW path at the top of build.bat, or make sure
    echo     gcc/windres are on your PATH and run:  gcc ... ^&^&  windres ...
    REM fall back to PATH
    set "GCC=gcc"
    set "WINDRES=windres"
) else (
    REM MSYS2 gcc must find cc1 and its DLLs on PATH
    set "PATH=%MINGW%;%PATH%"
)

cd /d "%~dp0"

echo [1/2] Compiling resources (icon + manifest + version)...
"%WINDRES%" app.rc -O coff -o app.res
if errorlevel 1 goto :fail

echo [2/2] Compiling and linking Look20.exe ...
"%GCC%" -O2 -municode -mwindows -o Look20.exe main.c app.res ^
    -lgdi32 -lshell32 -ladvapi32 -luser32 -lkernel32 -lwinmm
if errorlevel 1 goto :fail

del app.res >nul 2>&1
echo.
echo [OK] Built Look20.exe
echo     Run it, then look for the eye icon in your system tray.
goto :eof

:fail
echo.
echo [FAILED] Build error - see messages above.
exit /b 1
