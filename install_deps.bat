@echo off
setlocal enabledelayedexpansion

echo ========================================
echo GBServer - Install Dependencies
echo ========================================
echo.

:: Check Python
echo [1/3] Checking Python...
python --version >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Python is not installed or not in PATH
    echo Please install Python 3.7+ from https://python.org
    pause
    exit /b 1
)
echo [OK] Python found
echo.

:: Check Conan
echo [2/3] Checking Conan...
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

:: Prepare arguments for setup.bat
if "%~1"=="" (
    echo [INFO] No arguments provided for third-party installation.
    set /p "USER_ARGS=Enter arguments (e.g., --profile "myprofile.profile" [Please use the absolute path]): "
    if "!USER_ARGS!"=="" (
        echo [WARNING] No arguments entered, proceeding without arguments.
        set "SETUP_ARGS="
    ) else (
        set "SETUP_ARGS=!USER_ARGS!"
    )
) else (
    set "SETUP_ARGS=%*"
)

:: Install third-party packages
echo [3/3] Installing third-party packages...
if exist "3rd\setup.bat" (
    pushd 3rd
    if defined SETUP_ARGS (
        call setup.bat !SETUP_ARGS!
    ) else (
        call setup.bat
    )
    if errorlevel 1 (
        popd
        echo [ERROR] Failed to install third-party packages
        pause
        exit /b 1
    )
    popd
) else (
    echo [WARNING] 3rd\setup.bat not found, skipping third-party installation.
)
echo.


echo ========================================
echo [SUCCESS] All dependencies installed!
echo ========================================
echo.
echo Next steps:
echo   conan install . --build=missing -s build_type=Debug
echo   cmake --preset conan-debug
echo   cmake --build --preset conan-debug
echo.
pause
