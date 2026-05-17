@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

echo ========================================
echo GBServer - Uninstall Third-Party Packages
echo ========================================
echo.

set /p confirm="Are you sure? This will remove gbluasocket and gbnet! (y/N): "
if /i not "%confirm%"=="y" (
    echo Cancelled.
    pause
    exit /b 0
)
echo.

:: 卸载第三方包
echo [1/2] Uninstalling third-party packages...
cd 3rd
call uninstall.bat
cd ..
echo.

:: 删除 build 目录（可选）
echo [2/2] Removing build directory...
if exist "build" (
    rmdir /s /q build
    echo Build directory removed
) else (
    echo No build directory found
)

echo.
echo ========================================
echo [SUCCESS] Third-party packages removed!
echo ========================================
echo.
echo Note: Conan itself is still installed.
echo.
pause