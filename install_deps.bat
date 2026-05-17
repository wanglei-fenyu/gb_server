@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

echo ========================================
echo GBServer - Install Dependencies
echo ========================================
echo.

:: 检查 Python
echo [1/4] Checking Python...
python --version >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Python is not installed or not in PATH
    echo Please install Python 3.7+ from https://python.org
    pause
    exit /b 1
)
echo [OK] Python found
echo.

:: 检查 Conan
echo [2/4] Checking Conan...
conan --version >nul 2>&1
if errorlevel 1 (
    echo [INFO] Conan not found, installing...
    pip install conan
    if errorlevel 1 (
        echo [ERROR] Failed to install Conan
        pause
        exit /b 1
    )
)
echo [OK] Conan found
echo.

:: 安装第三方包
echo [3/4] Installing third-party packages...
cd 3rd
call setup.bat
if errorlevel 1 (
    cd ..
    echo [ERROR] Failed to install third-party packages
    pause
    exit /b 1
)
cd ..
echo.

:: 安装 Conan 依赖
echo [4/4] Installing Conan dependencies...
conan install . --build=missing
if errorlevel 1 (
    echo [ERROR] Failed to install Conan dependencies
    pause
    exit /b 1
)
echo.

echo ========================================
echo [SUCCESS] All dependencies installed!
echo ========================================
echo.
echo Next steps:
echo   cmake --preset conan-default
echo   cmake --build --preset conan-release
echo.
pause