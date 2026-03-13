@echo off
echo ============================================
echo   TiggyOpenMesh Gateway — Build Executables
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

echo [2/3] Building TiggyOpenMesh-Hub.exe...
%PYTHON% -m PyInstaller --noconfirm --onefile --windowed ^
    --collect-data customtkinter ^
    --name "TiggyOpenMesh-Hub" ^
    gateway_hub_gui.py
if errorlevel 1 (
    echo ERROR: Failed to build Hub executable.
    pause
    exit /b 1
)
echo.

echo [3/3] Building TiggyOpenMesh-Gateway.exe...
%PYTHON% -m PyInstaller --noconfirm --onefile --windowed ^
    --collect-data customtkinter ^
    --name "TiggyOpenMesh-Gateway" ^
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
echo     - TiggyOpenMesh-Hub.exe
echo     - TiggyOpenMesh-Gateway.exe
echo ============================================
pause
