@echo off
setlocal enabledelayedexpansion

REM Quick Conan installer for this repo.
REM Default is tuned for Windows/Linux compatibility with explicit host/build profiles.

set "PROFILE_HOST=profiles/msvc_debug_pr"
set "PROFILE_BUILD=profiles/msvc_debug_pr"
set "CPPSTD=23"
set "BUILD_MISSING=1"
set "DO_CLEAN=0"
set "EXTRA_ARGS="

:parse_args
if "%~1"=="" goto args_done

if /I "%~1"=="--clean" (
  set "DO_CLEAN=1"
  shift
  goto parse_args
)

if /I "%~1"=="--profile" (
  set "PROFILE_HOST=%~2"
  set "PROFILE_BUILD=%~2"
  shift
  shift
  goto parse_args
)

if /I "%~1"=="--build-profile" (
  set "PROFILE_BUILD=%~2"
  shift
  shift
  goto parse_args
)

if /I "%~1"=="--cppstd" (
  set "CPPSTD=%~2"
  shift
  shift
  goto parse_args
)

if /I "%~1"=="--no-build-missing" (
  set "BUILD_MISSING=0"
  shift
  goto parse_args
)

if /I "%~1"=="-h" goto help
if /I "%~1"=="--help" goto help

set "EXTRA_ARGS=!EXTRA_ARGS! %~1"
shift
goto parse_args

:help
echo Usage:
echo   conan_install.bat [options] [extra conan args]
echo.
echo Options:
echo   --clean                    Clear local Conan cache before install
echo   --profile ^<path^>           Host profile ^(default: profiles/clang_debug_pr^)
echo   --build-profile ^<path^>     Build profile ^(default: same as host^)
echo   --cppstd ^<value^>           Host compiler.cppstd ^(default: gnu17^)
echo   --no-build-missing         Do not use --build=missing
echo   -h, --help                 Show this help
exit /b 0

:args_done
where conan >nul 2>&1
if errorlevel 1 (
  echo [ERROR] conan not found in PATH
  exit /b 1
)

echo == Conan quick install ==
echo Host profile : %PROFILE_HOST%
echo Build profile: %PROFILE_BUILD%
echo cppstd       : %CPPSTD%

if "%DO_CLEAN%"=="1" (
  echo [1/2] Cleaning local Conan cache...
  conan remove "*" -c
  if errorlevel 1 exit /b 1
  echo [OK] Cache cleaned
)

echo [2/2] Installing dependencies...
set "CMD=conan install . -pr:h=%PROFILE_HOST% -pr:b=%PROFILE_BUILD% -s:h compiler.cppstd=%CPPSTD%"
if "%BUILD_MISSING%"=="1" set "CMD=%CMD% --build=missing"
set "CMD=%CMD%%EXTRA_ARGS%"

echo Running: %CMD%
call %CMD%
if errorlevel 1 exit /b 1

echo [SUCCESS] Conan install finished
exit /b 0
