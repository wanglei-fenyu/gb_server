@echo off
chcp 65001 >nul
python setup.py %*
exit /b %errorlevel%