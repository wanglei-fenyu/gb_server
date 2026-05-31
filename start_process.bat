@echo off
setlocal enabledelayedexpansion

set "PROJECT_ROOT=%~dp0"
set RES_PATH=%PROJECT_ROOT%res
set BUILD_TYPE=
set EXE_NAME=
set APP_TYPE_VALUE=

:: Server presets:  server_name -> exe_name,app_type_value
:: Usage: start_process.bat -s <server_name> -t <Debug|Release>

:loop
if "%~1"=="" goto :check_args
if /i "%~1"=="-t" set "BUILD_TYPE=%~2" & shift & shift & goto :loop
if /i "%~1"=="--type" set "BUILD_TYPE=%~2" & shift & shift & goto :loop
if /i "%~1"=="-n" set "EXE_NAME=%~2" & shift & shift & goto :loop
if /i "%~1"=="--name" set "EXE_NAME=%~2" & shift & shift & goto :loop
if /i "%~1"=="-s" set "SERVER_PRESET=%~2" & shift & shift & goto :loop
if /i "%~1"=="--server" set "SERVER_PRESET=%~2" & shift & shift & goto :loop
if /i "%~1"=="-h" goto :show_help
if /i "%~1"=="--help" goto :show_help
echo Unknown option: %~1
goto :show_help

:check_args
if "%BUILD_TYPE%"=="" (
    echo Build type not specified.
    :ask_type
    set /p "BUILD_TYPE=Enter build type (Debug/Release): "
    if /i "!BUILD_TYPE!"=="Debug" goto :type_ok
    if /i "!BUILD_TYPE!"=="Release" goto :type_ok
    echo Invalid input. Please enter 'Debug' or 'Release'.
    goto :ask_type
)
:type_ok

:: Resolve server preset if -s was given
if defined SERVER_PRESET (
    if /i "!SERVER_PRESET!"=="login" (
        set "EXE_NAME=login_server.exe"
        set "APP_TYPE_VALUE=8"
    ) else if /i "!SERVER_PRESET!"=="gateway" (
        set "EXE_NAME=gateway_server.exe"
        set "APP_TYPE_VALUE=64"
    ) else if /i "!SERVER_PRESET!"=="scene" (
        set "EXE_NAME=scene_server.exe"
        set "APP_TYPE_VALUE=32"
    ) else if /i "!SERVER_PRESET!"=="server_test" (
        set "EXE_NAME=server_test.exe"
        set "APP_TYPE_VALUE=1"
    ) else (
        echo Unknown server preset: !SERVER_PRESET!
        echo Available: login, gateway, scene, server_test
        pause
        exit /b 1
    )
)

:: Fallback: prompt for EXE_NAME if still empty
if "%EXE_NAME%"=="" (
    echo Executable name not specified.
    set /p "EXE_NAME=Enter executable name (e.g., server_test.exe): "
    if "!EXE_NAME!"=="" (
        echo Error: Executable name cannot be empty.
        pause
        exit /b 1
    )
)

:: Default app_type to 0 (global) if not resolved from preset
if "%APP_TYPE_VALUE%"=="" set "APP_TYPE_VALUE=0"

set "CLIENT_EXE=%PROJECT_ROOT%build\%BUILD_TYPE%\%BUILD_TYPE%\bin\%EXE_NAME%"

echo Starting server...
echo Build type: %BUILD_TYPE%
echo Executable: %CLIENT_EXE%
echo Resource:   %RES_PATH%
echo.

if not exist "%CLIENT_EXE%" (
    echo ERROR: Executable not found at %CLIENT_EXE%
    pause
    exit /b 1
)

"%CLIENT_EXE%" -t %APP_TYPE_VALUE% -r "%RES_PATH%"
pause
exit /b 0

:show_help
echo Usage: %~nx0 [OPTIONS]
echo.
echo Options:
echo   -s, --server NAME   Server preset: login, gateway, scene, server_test
echo   -t, --type TYPE     Build type: Debug or Release
echo   -n, --name NAME     Executable name (e.g., server_test.exe)
echo   -h, --help          Show this help message
echo.
echo Examples:
echo   %~nx0 -s login -t Debug
echo   %~nx0 -s gateway -t Debug
echo   %~nx0 -s scene -t Debug
echo   %~nx0 -n client_test.exe -t Debug
pause
exit /b 0
