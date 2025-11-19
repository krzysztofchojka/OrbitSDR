@echo off
echo Kompilacja projektu SDR na Windows...

REM --- KONFIGURACJA ŚCIEŻEK (Zmień to na swoje ścieżki do SFML!) ---
SET SFML_INCLUDE="C:\SFML\include"
SET SFML_LIB="C:\SFML\lib"

REM --- KOMPILACJA ---
REM Dodajemy -lcomdlg32 dla obsługi okien dialogowych
g++ -std=c++17 -O3 -I%SFML_INCLUDE% src/main.cpp -o orbitsdr.exe -L%SFML_LIB% -lsfml-audio -lsfml-graphics -lsfml-window -lsfml-system -lcomdlg32

if %errorlevel% neq 0 (
    echo Blad kompilacji!
    pause
    exit /b %errorlevel%
)

echo Kompilacja zakonczona sukcesem!
echo Upewnij sie, ze pliki .dll z folderu SFML/bin sa obok orbitsdr.exe!
pause