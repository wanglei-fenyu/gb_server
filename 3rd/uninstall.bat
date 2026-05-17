@echo off
chcp 65001 >nul
python uninstall.py %*
exit /b %errorlevel%