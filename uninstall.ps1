# DevAI 卸载器：原样还原 Dev-C++ 的编译器和运行器
param(
    [string]$DevAIHome = "",
    [switch]$Purge,
    [switch]$NoWait
)

$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$DevAIHomeDir = if ($DevAIHome) { $DevAIHome } elseif (Test-Path 'D:\') { 'D:\devai' } else { Join-Path $env:LOCALAPPDATA 'DevAI' }
$StateFile = Join-Path $DevAIHomeDir 'install-state.json'

$Host.UI.RawUI.WindowTitle = 'DevAI 卸载器'

Write-Host '=================================================' -ForegroundColor DarkCyan
Write-Host '   DevAI 小助手  ——  正在卸载并还原 Dev-C++' -ForegroundColor White
Write-Host '=================================================' -ForegroundColor DarkCyan

# ---------------------------------------------------- 读取安装记录
$state = $null
if (Test-Path $StateFile) {
    try { $state = Get-Content $StateFile -Raw -Encoding UTF8 | ConvertFrom-Json } catch {}
}

$DevCppRoot = $null
if ($state) { $DevCppRoot = $state.devCppRoot }

if (-not $DevCppRoot) {
    $cands = @('D:\dev\Dev-Cpp','D:\Dev-Cpp','D:\Program Files\Dev-Cpp',
               'C:\Program Files\Dev-Cpp','C:\Program Files (x86)\Dev-Cpp')
    Get-Process DevCpp -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.Path) { $cands = @((Split-Path -Parent $_.Path)) + $cands }
    }
    foreach ($c in $cands) {
        if ($c -and (Test-Path (Join-Path $c 'DevCpp.exe'))) { $DevCppRoot = (Resolve-Path $c).Path; break }
    }
}
if (-not $DevCppRoot) {
    Write-Host '[DevAI] 找不到 Dev-C++ 安装目录，无法还原。' -ForegroundColor Red
    if (-not $NoWait) { Start-Sleep 15 }
    exit 1
}

Write-Host "  Dev-C++ 目录：$DevCppRoot" -ForegroundColor Gray
$MingwBin = Join-Path $DevCppRoot 'MinGW64\bin'

# ---------------------------------------------------- 关闭 Dev-C++ / 助手
$running = Get-Process DevCpp -ErrorAction SilentlyContinue
if ($running) {
    Write-Host '  Dev-C++ 正在运行，请先关闭它。' -ForegroundColor Yellow
    Read-Host '  关闭后按回车继续'
}

$pidFile = Join-Path $DevAIHomeDir 'agent.pid'
if (Test-Path $pidFile) {
    $agentPid = (Get-Content $pidFile -Raw).Trim()
    if ($agentPid -match '^\d+$') {
        try { Stop-Process -Id ([int]$agentPid) -Force -ErrorAction SilentlyContinue } catch {}
    }
    Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
}

# ---------------------------------------------------- 还原编译器
$gcc = Join-Path $MingwBin 'gcc.exe'
$gpp = Join-Path $MingwBin 'g++.exe'
$gccReal = Join-Path $MingwBin 'gcc-real.exe'
$gppReal = Join-Path $MingwBin 'g++-real.exe'

if (Test-Path $gccReal) {
    Remove-Item $gcc -Force -ErrorAction SilentlyContinue
    Rename-Item $gccReal 'gcc.exe'
    Write-Host '  已还原 gcc.exe' -ForegroundColor Green
} else {
    Write-Host '  gcc.exe 本来就没被动过，跳过' -ForegroundColor DarkGray
}

if (Test-Path $gppReal) {
    Remove-Item $gpp -Force -ErrorAction SilentlyContinue
    Rename-Item $gppReal 'g++.exe'
    Write-Host '  已还原 g++.exe' -ForegroundColor Green
} else {
    Write-Host '  g++.exe 本来就没被动过，跳过' -ForegroundColor DarkGray
}

# ---------------------------------------------------- 还原 ConsolePauser
$cp = Join-Path $DevCppRoot 'ConsolePauser.exe'
$cpOrig = Join-Path $DevCppRoot 'ConsolePauser.orig.exe'
if (Test-Path $cpOrig) {
    Copy-Item $cpOrig $cp -Force
    Write-Host '  已还原 ConsolePauser.exe（原文件保留为 ConsolePauser.orig.exe）' -ForegroundColor Green
} else {
    Write-Host '  ConsolePauser.exe 没有备份，跳过' -ForegroundColor DarkGray
}

# ---------------------------------------------------- 清除安装目录标记
foreach ($mk in @((Join-Path $MingwBin 'devai-home.txt'), (Join-Path $DevCppRoot 'devai-home.txt'))) {
    if (Test-Path $mk) { Remove-Item $mk -Force -ErrorAction SilentlyContinue }
}

# ---------------------------------------------------- 开始菜单快捷方式
try {
    $lnk = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Dev-C++ 智能版.lnk'
    if (Test-Path $lnk) { Remove-Item $lnk -Force; Write-Host '  已删除开始菜单快捷方式' -ForegroundColor Green }
} catch {}

# ---------------------------------------------------- 运行目录
if ($Purge) {
    Write-Host '  正在删除运行目录（含 API Key 配置）...' -ForegroundColor Yellow
    Remove-Item $DevAIHomeDir -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host '  已删除' -ForegroundColor Green
} else {
    Write-Host "  运行目录保留在 $DevAIHomeDir （API Key 等配置还在，想彻底删除请用 -Purge）" -ForegroundColor Gray
}

Write-Host ''
Write-Host '  卸载完成，Dev-C++ 已恢复原样。' -ForegroundColor White
Write-Host '=================================================' -ForegroundColor DarkCyan
if (-not $NoWait) {
    Write-Host '  本窗口 15 秒后自动关闭 ...' -ForegroundColor DarkGray
    Start-Sleep -Seconds 15
}
