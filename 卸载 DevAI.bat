@echo off
chcp 936 >nul
title DevAI 卸载器 - 还原 Dev-C++
cd /d "%~dp0"
echo 正在卸载，请稍候...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1"
if errorlevel 1 pause
exit