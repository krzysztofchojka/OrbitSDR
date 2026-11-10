@echo off
title OrbitSDR build (MinGW)

:: Default MSYS2 install path (change it if you installed MSYS2 elsewhere)
set MSYS2_DIR=C:\msys64

if not exist "%MSYS2_DIR%\msys2_shell.cmd" (
    echo ERROR: MSYS2 installation not found in %MSYS2_DIR%.
    echo Open build_win.bat in a text editor and fix the MSYS2_DIR variable.
    pause
    exit /b 1
)

echo Starting the MSYS2 UCRT64 environment...
echo.

:: msys2_shell.cmd flags:
:: -ucrt64   : force the UCRT64 environment
:: -defterm  : use the default terminal instead of opening a new MinTTY window
:: -no-start : run in the same window so the logs stay visible afterwards
:: -here     : use the folder containing this .bat as the working directory
:: -c        : run the given command in bash and wait for a key press at the end

"%MSYS2_DIR%\msys2_shell.cmd" -ucrt64 -defterm -no-start -here -c "./build_mingw_local.sh; echo ''; read -n 1 -s -r -p 'Build finished. Press any key to close...'"