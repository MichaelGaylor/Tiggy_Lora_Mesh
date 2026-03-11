@echo off
echo ============================================
echo   LoRa Mesh Gateway — Build Executables
echo ============================================
echo.

set PYTHON=python

echo [1/3] Installing dependencies...
%PYTHON% -m pip install -r requirements.txt
if errorlevel 1 (
    echo ERROR: Failed to install dependencies.
    pause
    exit /b 1
)
echo.

echo [2/3] Building LoRa-Mesh-Hub.exe...
%PYTHON% -m PyInstaller --noconfirm --onefile --windowed ^
    --collect-data customtkinter ^
    --name "LoRa-Mesh-Hub" ^
    gateway_hub_gui.py
if errorlevel 1 (
    echo ERROR: Failed to build Hub executable.
    pause
    exit /b 1
)
echo.

echo [3/3] Building LoRa-Mesh-Gateway.exe...
%PYTHON% -m PyInstaller --noconfirm --onefile --windowed ^
    --collect-data customtkinter ^
    --name "LoRa-Mesh-Gateway" ^
    gateway_gui.py
if errorlevel 1 (
    echo ERROR: Failed to build Gateway executable.
    pause
    exit /b 1
)
echo.

echo ============================================
echo   Build complete!
echo   Executables are in: dist\
echo     - LoRa-Mesh-Hub.exe
echo     - LoRa-Mesh-Gateway.exe
echo ============================================
pause
