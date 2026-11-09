@echo off
chcp 936 >nul
title DevAI 安装器 - 自动检测 Dev-C++
cd /d "%~dp0"
echo 正在启动安装程序，请稍候...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
if errorlevel 1 (
  echo.
  echo [DevAI] 安装过程出错，请把上面的信息反馈给开发者。
  pause
)
exit