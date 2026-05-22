@echo off
setlocal enabledelayedexpansion

:: 项目根目录
set PROJECT_ROOT=%USERPROFILE%\workspace\gb_server
set RES_PATH=%PROJECT_ROOT%\res

:: 初始化变量
set BUILD_TYPE=
set EXE_NAME=

:: 解析命令行参数
:parse_args
if "%~1"=="" goto :check_args
if /i "%~1"=="-t" set BUILD_TYPE=%~2& shift & shift & goto :parse_args
if /i "%~1"=="--type" set BUILD_TYPE=%~2& shift & shift & goto :parse_args
if /i "%~1"=="-n" set EXE_NAME=%~2& shift & shift & goto :parse_args
if /i "%~1"=="--name" set EXE_NAME=%~2& shift & shift & goto :parse_args
if /i "%~1"=="-h" goto :show_help
if /i "%~1"=="--help" goto :show_help
echo Unknown option: %~1
goto :show_help

:show_help
echo Usage: %~nx0 [OPTIONS]
echo Options:
echo   -t, --type TYPE    Build type: Debug or Release (default: prompt if missing)
echo   -n, --name NAME    Executable name, e.g., server_test (default: prompt if missing)
echo   -h, --help         Show this help message
pause
exit /b 0

:check_args
:: 交互获取构建类型
if "%BUILD_TYPE%"=="" (
    echo Build type not specified.
    :ask_type
    set /p BUILD_TYPE="Enter build type (Debug/Release): "
    if /i "!BUILD_TYPE!"=="Debug" goto :type_ok
    if /i "!BUILD_TYPE!"=="Release" goto :type_ok
    echo Invalid input. Please enter 'Debug' or 'Release'.
    goto :ask_type
)
:type_ok

:: 交互获取可执行文件名
if "%EXE_NAME%"=="" (
    echo Executable name not specified.
    set /p EXE_NAME="Enter executable name (e.g., server_test): "
    if "!EXE_NAME!"=="" (
        echo Error: Executable name cannot be empty.
        pause
        exit /b 1
    )
)

:: 构建完整路径（注意两个 %BUILD_TYPE% 层级）
set CLIENT_EXE=%PROJECT_ROOT%\build\%BUILD_TYPE%\%BUILD_TYPE%\bin\%EXE_NAME%

echo Starting client...
echo Build type: %BUILD_TYPE%
echo Executable: %CLIENT_EXE%
echo Resource: %RES_PATH%
echo.

:: 检查文件是否存在
if not exist "%CLIENT_EXE%" (
    echo ERROR: Client not found at %CLIENT_EXE%
    pause
    exit /b 1
)

:: 运行客户端
"%CLIENT_EXE%" -t 1 -r "%RES_PATH%"

pause
