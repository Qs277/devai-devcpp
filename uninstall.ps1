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

function Find-MingwBin([string]$root) {
    foreach ($n in @('MinGW64\bin','mingw64\bin','MinGW64-13.2\bin','mingw32\bin')) {
        $p = Join-Path $root $n
        if ((Test-Path (Join-Path $p 'gcc.exe')) -or (Test-Path (Join-Path $p 'gcc-real.exe'))) { return $p }
    }
    foreach ($d in (Get-ChildItem $root -Directory -ErrorAction SilentlyContinue)) {
        $p = Join-Path $d.FullName 'bin'
        if ((Test-Path (Join-Path $p 'gcc.exe')) -or (Test-Path (Join-Path $p 'gcc-real.exe'))) { return $p }
    }
    return $null
}

# 安装记录可能是多版本（installs 数组），也可能是老版的单条记录
$roots = @()
if ($state) {
    if ($state.installs) {
        foreach ($i in $state.installs) { if ($i.devCppRoot) { $roots += $i } }
    } elseif ($state.devCppRoot) {
        $roots += [pscustomobject]@{ devCppRoot = $state.devCppRoot; mingwBin = $state.mingwBin }
    }
}
if ($roots.Count -eq 0) {
    $cands = @('D:\dev\Dev-Cpp','D:\Dev-Cpp','D:\Program Files\Dev-Cpp',
               'C:\Program Files\Dev-Cpp','C:\Program Files (x86)\Dev-Cpp',
               'D:\最新的dev\Dev-Cpp','D:\最新的dev')
    Get-Process DevCpp -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.Path) { $cands = @((Split-Path -Parent $_.Path)) + $cands }
    }
    foreach ($c in $cands) {
        if ($c -and (Test-Path (Join-Path $c 'DevCpp.exe'))) {
            $roots += [pscustomobject]@{ devCppRoot = (Resolve-Path $c).Path.TrimEnd('\'); mingwBin = $null }
            break
        }
    }
}
if ($roots.Count -eq 0) {
    Write-Host '[DevAI] 找不到 Dev-C++ 安装目录，无法还原。' -ForegroundColor Red
    if (-not $NoWait) { Start-Sleep 15 }
    exit 1
}

foreach ($r in $roots) { Write-Host "  Dev-C++ 目录：$($r.devCppRoot)" -ForegroundColor Gray }

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

# ---------------------------------------------------- 还原编译器（可能有多个版本）
foreach ($r in $roots) {
    $root = $r.devCppRoot
    if (-not (Test-Path $root)) {
        Write-Host "  $root 已不存在，跳过" -ForegroundColor DarkGray
        continue
    }
    $bin = $r.mingwBin
    if (-not $bin -or -not (Test-Path $bin)) { $bin = Find-MingwBin $root }
    if (-not $bin) {
        Write-Host "  $root 里找不到编译器目录，跳过" -ForegroundColor DarkGray
        continue
    }

    Write-Host "  正在还原：$root" -ForegroundColor Gray
    $gcc = Join-Path $bin 'gcc.exe'
    $gpp = Join-Path $bin 'g++.exe'
    $gccReal = Join-Path $bin 'gcc-real.exe'
    $gppReal = Join-Path $bin 'g++-real.exe'

    if (Test-Path $gccReal) {
        Remove-Item $gcc -Force -ErrorAction SilentlyContinue
        Rename-Item $gccReal 'gcc.exe'
        Write-Host '    已还原 gcc.exe' -ForegroundColor Green
    } else {
        Write-Host '    gcc.exe 本来就没被动过，跳过' -ForegroundColor DarkGray
    }

    if (Test-Path $gppReal) {
        Remove-Item $gpp -Force -ErrorAction SilentlyContinue
        Rename-Item $gppReal 'g++.exe'
        Write-Host '    已还原 g++.exe' -ForegroundColor Green
    } else {
        Write-Host '    g++.exe 本来就没被动过，跳过' -ForegroundColor DarkGray
    }

    # 还原 ConsolePauser
    $cp = Join-Path $root 'ConsolePauser.exe'
    $cpOrig = Join-Path $root 'ConsolePauser.orig.exe'
    if (Test-Path $cpOrig) {
        Copy-Item $cpOrig $cp -Force
        Write-Host '    已还原 ConsolePauser.exe' -ForegroundColor Green
    } else {
        Write-Host '    ConsolePauser.exe 没有备份，跳过' -ForegroundColor DarkGray
    }

    # 清除安装目录标记
    foreach ($mk in @((Join-Path $bin 'devai-home.txt'), (Join-Path $root 'devai-home.txt'))) {
        if (Test-Path $mk) { Remove-Item $mk -Force -ErrorAction SilentlyContinue }
    }
}

# ---------------------------------------------------- 开始菜单快捷方式
try {
    $sm = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
    foreach ($lnk in (Get-ChildItem $sm -Filter 'Dev-C++ 智能版*.lnk' -ErrorAction SilentlyContinue)) {
        Remove-Item $lnk.FullName -Force -ErrorAction SilentlyContinue
        Write-Host '  已删除开始菜单快捷方式' -ForegroundColor Green
    }
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
