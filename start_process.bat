@echo off
set PROJECT_ROOT=D:\work_space\gb_server
set CLIENT_EXE=%PROJECT_ROOT%\build\debug\bin\server_test.exe
set RES_PATH=%PROJECT_ROOT%\res

echo Starting client...
echo Executable: %CLIENT_EXE%
echo Resource: %RES_PATH%
echo.

if not exist "%CLIENT_EXE%" (
    echo ERROR: Client not found at %CLIENT_EXE%
    pause
    exit /b 1
)

"%CLIENT_EXE%" -t 1 -r "%RES_PATH%"
pause
