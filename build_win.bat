@echo off
title Kompilacja OrbitSDR (MinGW)

:: Domyslna sciezka instalacji MSYS2 (zmien, jesli zainstalowales w innym miejscu)
set MSYS2_DIR=C:\msys64

if not exist "%MSYS2_DIR%\msys2_shell.cmd" (
    echo BLAD: Nie znaleziono instalacji MSYS2 w folderze %MSYS2_DIR%.
    echo Otworz plik run_build.bat w notatniku i popraw zmienna MSYS2_DIR.
    pause
    exit /b 1
)

echo Uruchamianie srodowiska MSYS2 UCRT64...
echo.

:: Flagi msys2_shell.cmd:
:: -ucrt64   : wymusza srodowisko UCRT64
:: -defterm  : uzywa domyslnego terminala zamiast otwierac nowe okno MinTTY
:: -no-start : uruchamia w tym samym oknie, zeby zachowac logi po zakonczeniu
:: -here     : ustawia katalog roboczy na ten, w ktorym znajduje sie plik .bat
:: -c        : przekazuje komende do wykonania w bashu i czeka na reakcje na koncu

"%MSYS2_DIR%\msys2_shell.cmd" -ucrt64 -defterm -no-start -here -c "./build_mingw_local.sh; echo ''; read -n 1 -s -r -p 'Kompilacja zakonczona. Nacisnij dowolny klawisz, aby zamknac...'"