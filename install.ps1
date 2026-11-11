# DevAI 安装器：自动检测本机 Dev-C++，接入 AI 编译/运行助手
# 用法：右键 安装 DevAI.bat → 以管理员身份运行（普通权限一般也够）
param(
    [string]$DevAIHome = "",
    [string]$DevCppRoot = "",   # 指定 Dev-C++ 目录（可装到任意版本，例如 D:\最新的dev\Dev-Cpp）
    [switch]$All,               # 自动找出本机所有 Dev-C++，每个都接一遍
    [switch]$SkipWait
)

$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$DevAIHomeDir = if ($DevAIHome) { $DevAIHome } elseif (Test-Path 'D:\') { 'D:\devai' } else { Join-Path $env:LOCALAPPDATA 'DevAI' }
$StateFile = Join-Path $DevAIHomeDir 'install-state.json'
$Utf8 = New-Object System.Text.UTF8Encoding($false)

function Write-Step($t) { Write-Host "`n[DevAI] $t" -ForegroundColor Cyan }
function Write-Ok($t)   { Write-Host "        $t" -ForegroundColor Green }
function Write-Warn2($t){ Write-Host "        $t" -ForegroundColor Yellow }

$Host.UI.RawUI.WindowTitle = 'DevAI 安装器 - 自动检测 Dev-C++'

Write-Host '=================================================' -ForegroundColor DarkCyan
Write-Host '   DevAI 小助手  ——  Dev-C++ 智能报错讲解插件' -ForegroundColor White
Write-Host '=================================================' -ForegroundColor DarkCyan

# ------------------------------------------------------------------ 1. 找 Dev-C++
Write-Step '步骤 1/6  正在自动检测本机的 Dev-C++ ...'

function Get-DevCppRoots {
    $cands = New-Object System.Collections.ArrayList

    $regRoots = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*'
    )
    foreach ($r in $regRoots) {
        try {
            Get-ItemProperty $r -ErrorAction SilentlyContinue |
                Where-Object { $_.DisplayName -and ($_.DisplayName -match 'Dev-C\+' -or $_.DisplayName -match 'Dev-Cpp' -or $_.DisplayName -match 'Bloodshed') } |
                ForEach-Object {
                    if ($_.InstallLocation) { [void]$cands.Add(($_.InstallLocation).TrimEnd('\')) }
                    if ($_.DisplayIcon) {
                        $p = ($_.DisplayIcon -split ',')[0].Trim('"')
                        if (Test-Path $p) { [void]$cands.Add((Split-Path -Parent $p)) }
                    }
                }
        } catch {}
    }

    Get-Process DevCpp -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.Path) { [void]$cands.Add((Split-Path -Parent $_.Path)) }
    }

    foreach ($c in @('D:\dev\Dev-Cpp','D:\Dev-Cpp','D:\Program Files\Dev-Cpp',
                     'C:\Program Files\Dev-Cpp','C:\Program Files (x86)\Dev-Cpp',
                     "$env:LOCALAPPDATA\Programs\Dev-Cpp",
                     'D:\最新的dev\Dev-Cpp','D:\最新的dev')) {
        [void]$cands.Add($c)
    }

    $found = New-Object System.Collections.ArrayList
    foreach ($c in $cands) {
        if (-not $c) { continue }
        if (-not (Test-Path (Join-Path $c 'DevCpp.exe'))) { continue }
        $full = (Resolve-Path $c).Path.TrimEnd('\')
        if (-not ($found -contains $full)) { [void]$found.Add($full) }
    }
    return $found
}

# Dev-C++ 的编译器目录：老版叫 MinGW64\bin，新版（小龙版）叫 mingw64\bin，个别版本用 MinGW64-13.2
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

function Get-InstallLabel([string]$root) {
    $leaf = Split-Path $root -Leaf
    $parent = Split-Path (Split-Path $root -Parent) -Leaf
    $l = if ($parent) { "$parent-$leaf" } else { $leaf }
    return ($l -replace '[\\/:*?"<>|\s]', '_')
}

$rootList = @()
if ($DevCppRoot) {
    if (Test-Path (Join-Path $DevCppRoot 'DevCpp.exe')) { $rootList = @((Resolve-Path $DevCppRoot).Path.TrimEnd('\')) }
    else { Write-Warn2 "指定的目录里没有 DevCpp.exe：$DevCppRoot" }
} else {
    $rootList = @(Get-DevCppRoots)
}
if ($rootList.Count -eq 0) {
    Write-Warn2 '没有在注册表/常见目录里找到 Dev-C++。'
    $manual = Read-Host '请输入 Dev-C++ 安装目录（例如 D:\dev\Dev-Cpp，直接回车放弃）'
    if ($manual -and (Test-Path (Join-Path $manual 'DevCpp.exe'))) { $rootList = @((Resolve-Path $manual).Path.TrimEnd('\')) }
}
if ($rootList.Count -eq 0) { Write-Host '[DevAI] 未能定位 Dev-C++，安装中止。' -ForegroundColor Red; if(-not $SkipWait){ Start-Sleep 15 }; exit 1 }

if ($rootList.Count -gt 1 -and -not $All) {
    Write-Warn2 "检测到 $($rootList.Count) 个 Dev-C++，默认只接第一个（加 -All 可以全部接入）："
    foreach ($r in $rootList) { Write-Warn2 "  $r" }
    $rootList = @($rootList[0])
}

$targets = @()
foreach ($r in $rootList) {
    $bin = Find-MingwBin $r
    if ($bin) {
        $targets += [pscustomobject]@{ Root = $r; Bin = $bin; Label = (Get-InstallLabel $r) }
    } else {
        Write-Warn2 "$r 里没找到 MinGW 的 bin 目录（可能是精简版），跳过。"
    }
}
if ($targets.Count -eq 0) {
    Write-Host '[DevAI] 没有可接入的 Dev-C++ 编译器目录，安装中止。' -ForegroundColor Red
    if (-not $SkipWait) { Start-Sleep 15 }
    exit 1
}

# 第一个作为主安装（自检、状态文件里的主记录都用它）
$DevCppRoot = $targets[0].Root
$MingwBin  = $targets[0].Bin

foreach ($t in $targets) {
    $v = (Get-Item (Join-Path $t.Root 'DevCpp.exe')).VersionInfo
    $vt = if ($v.FileVersion) { $v.FileVersion } else { '未知版本' }
    Write-Ok "Dev-C++：$($t.Root)   （$vt，编译器 $($t.Bin)）"
}
$ver = (Get-Item (Join-Path $DevCppRoot 'DevCpp.exe')).VersionInfo
$verText = if ($ver.FileVersion) { $ver.FileVersion } else { '未知版本' }

# ------------------------------------------------------------------ 2. 检查 Dev-C++ 是否在运行
Write-Step '步骤 2/6  检查 Dev-C++ 是否正在运行 ...'
$running = Get-Process DevCpp -ErrorAction SilentlyContinue
if ($running) {
    Write-Warn2 'Dev-C++ 正在运行，接入编译器前需要先关闭它。'
    Read-Host '请手动关闭 Dev-C++ 窗口，然后按回车继续'
    $running = Get-Process DevCpp -ErrorAction SilentlyContinue
    if ($running) {
        Write-Host '[DevAI] Dev-C++ 仍在运行，安装中止（请关闭后重试）。' -ForegroundColor Red
        if (-not $SkipWait) { Start-Sleep 15 }
        exit 1
    }
}
Write-Ok 'Dev-C++ 已关闭，可以安全接入'

# ------------------------------------------------------------------ 3. 准备目录
Write-Step '步骤 3/6  准备运行目录 ...'
foreach ($d in @($DevAIHomeDir, (Join-Path $DevAIHomeDir 'queue'), (Join-Path $DevAIHomeDir 'state'), (Join-Path $DevAIHomeDir 'backup'), (Join-Path $DevAIHomeDir 'bin'), (Join-Path $DevAIHomeDir 'server'), (Join-Path $DevAIHomeDir 'server\ui'))) {
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
}
Copy-Item (Join-Path $PluginRoot 'server\server.js') (Join-Path $DevAIHomeDir 'server\server.js') -Force
Copy-Item (Join-Path $PluginRoot 'server\ui\index.html') (Join-Path $DevAIHomeDir 'server\ui\index.html') -Force
Copy-Item (Join-Path $PluginRoot 'bin\devai_gcc.exe') (Join-Path $DevAIHomeDir 'bin\devai_gcc.exe') -Force
Copy-Item (Join-Path $PluginRoot 'bin\devai_gpp.exe') (Join-Path $DevAIHomeDir 'bin\devai_gpp.exe') -Force
Copy-Item (Join-Path $PluginRoot 'bin\devai_console.exe') (Join-Path $DevAIHomeDir 'bin\devai_console.exe') -Force
Copy-Item (Join-Path $PluginRoot 'bin\devai_dock.exe') (Join-Path $DevAIHomeDir 'bin\devai_dock.exe') -Force
Copy-Item (Join-Path $PluginRoot 'bin\devai_glass.exe') (Join-Path $DevAIHomeDir 'bin\devai_glass.exe') -Force
if (Test-Path (Join-Path $PluginRoot 'bin\open-panel.ps1')) {
    Copy-Item (Join-Path $PluginRoot 'bin\open-panel.ps1') (Join-Path $DevAIHomeDir 'bin\open-panel.ps1') -Force
}
Write-Ok "运行目录：$DevAIHomeDir（含窗口嵌入组件 devai_dock.exe / 玻璃浮层 devai_glass.exe）"

# Node 路径
$nodePath = ''
$cmd = Get-Command node -ErrorAction SilentlyContinue
if ($cmd) { $nodePath = $cmd.Source }
if (-not $nodePath) {
    foreach ($p in @('E:\node.exe', "$env:ProgramFiles\nodejs\node.exe", "${env:ProgramFiles(x86)}\nodejs\node.exe")) {
        if (Test-Path $p) { $nodePath = $p; break }
    }
}
if (-not $nodePath) {
    Write-Warn2 '没有检测到 Node.js！AI 助手需要 Node.js 才能运行。'
    Write-Warn2 '请先安装 https://nodejs.org （一路下一步即可），然后重新运行本安装器。'
    Read-Host '如果已经装在别处，请直接输入 node.exe 的完整路径（回车跳过）' | ForEach-Object { if ($_ -and (Test-Path $_)) { $script:nodePath = $_ } }
} else {
    Write-Ok "Node.js：$nodePath"
}
if ($nodePath) { [System.IO.File]::WriteAllText((Join-Path $DevAIHomeDir 'node_path.txt'), $nodePath, $Utf8) }

# 默认配置（已存在就不覆盖，保住用户填好的 Key）
$cfgFile = Join-Path $DevAIHomeDir 'config.json'
if (-not (Test-Path $cfgFile)) {
    $cfg = @{
        apiBase = 'https://api.deepseek.com'
        apiKey = ''
        model = 'deepseek-chat'
        temperature = 0.2
        maxTokens = 1200
        port = 47901
        autoOpenPanel = $true
        explainRuntime = $true
        explainWarnings = $true
    } | ConvertTo-Json
    [System.IO.File]::WriteAllText($cfgFile, $cfg, $Utf8)
    Write-Ok '已生成默认配置（DeepSeek，稍后在面板里填 API Key）'
}

# ------------------------------------------------------------------ 4. 接入编译器
Write-Step '步骤 4/6  接入编译器（可随时一键还原）...'

$gbk = [System.Text.Encoding]::GetEncoding(936)

foreach ($t in $targets) {
    $bin = $t.Bin
    $root = $t.Root
    $gcc = Join-Path $bin 'gcc.exe'
    $gpp = Join-Path $bin 'g++.exe'
    $gccReal = Join-Path $bin 'gcc-real.exe'
    $gppReal = Join-Path $bin 'g++-real.exe'

    # 真编译器改名（只做一次，之后重复安装不会重复改名）
    if ((Test-Path $gcc) -and (-not (Test-Path $gccReal))) { Rename-Item $gcc 'gcc-real.exe' }
    if ((Test-Path $gpp) -and (-not (Test-Path $gppReal))) { Rename-Item $gpp 'g++-real.exe' }

    # 放入拦截器，占用 gcc/g++ 名字
    Copy-Item (Join-Path $PluginRoot 'bin\devai_gcc.exe') $gcc -Force
    Copy-Item (Join-Path $PluginRoot 'bin\devai_gpp.exe') $gpp -Force
    Write-Ok "$bin  （gcc / g++）已接入 DevAI 拦截层"

    # 把安装目录写成标记文件：用户直接双击 Dev-C++（不走启动器、没有 DEVAI_HOME）时拦截器也能找到 DevAI
    [System.IO.File]::WriteAllText((Join-Path $bin 'devai-home.txt'), $DevAIHomeDir, $gbk)
    [System.IO.File]::WriteAllText((Join-Path $root 'devai-home.txt'), $DevAIHomeDir, $gbk)

    # 运行拦截：替换 ConsolePauser.exe（原文件备份为 ConsolePauser.orig.exe）
    $cp = Join-Path $root 'ConsolePauser.exe'
    $cpOrig = Join-Path $root 'ConsolePauser.orig.exe'
    if ((Test-Path $cp) -and (-not (Test-Path $cpOrig))) { Copy-Item $cp $cpOrig -Force }
    Copy-Item (Join-Path $PluginRoot 'bin\devai_console.exe') $cp -Force
    Write-Ok "$root  （ConsolePauser）已接入运行监控"
}
Write-Ok "已记录安装目录：$DevAIHomeDir"

# 做个冒烟测试：故意编译一段错代码，确认拦截器真的在工作
Write-Step '        正在自检编译器拦截层 ...'
$tmpDir = Join-Path $env:TEMP ('devai_test_' + [guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $tmpDir -Force | Out-Null
$badFile = Join-Path $tmpDir 'bad.cpp'
@'
#include <iostream>
using namespace std;
int main(){
    cout << "hi" << endl
    return 0;
}
'@ | Set-Content -Path $badFile -Encoding UTF8
$env:DEVAI_HOME = $DevAIHomeDir
$env:DEVAI_NO_AUTOSTART = '1'
$env:PATH = "$MingwBin;$env:PATH"
$p = Start-Process -FilePath (Join-Path $MingwBin 'g++.exe') -ArgumentList @($badFile, '-o', (Join-Path $tmpDir 'bad.exe')) -Wait -NoNewWindow -PassThru
$lastEvt = Get-ChildItem (Join-Path $DevAIHomeDir 'queue') -Filter '*_ce.kv' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($p.ExitCode -ne 0 -and $lastEvt) {
    Write-Ok '自检通过：编译器报错已被拦截并写入事件队列'
} elseif ($p.ExitCode -ne 0) {
    Write-Warn2 '编译器返回了错误，但没看到事件文件，拦截可能未生效。'
} else {
    Write-Warn2 '自检没有触发报错，可能拦截器没被调用（请把这段输出发给开发者）'
}
Remove-Item $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item Env:DEVAI_HOME -ErrorAction SilentlyContinue
Remove-Item Env:DEVAI_NO_AUTOSTART -ErrorAction SilentlyContinue

# ------------------------------------------------------------------ 5. 快捷启动
Write-Step '步骤 5/6  生成启动脚本与快捷方式 ...'

$enc936 = [System.Text.Encoding]::GetEncoding(936)
function Write-GbkFile([string]$path, [string]$content) {
    $content = $content.Replace("`r`n", "`n").Replace("`n", "`r`n")
    [System.IO.File]::WriteAllText($path, $content, $enc936)
}

$plainLauncher = Join-Path $DevAIHomeDir '启动 Dev-C++ 智能版.bat'
$launchers = @()
foreach ($t in $targets) {
$launcher = Join-Path $DevAIHomeDir ("启动 Dev-C++ 智能版 ({0}).bat" -f $t.Label)
$devRoot = $t.Root
Write-GbkFile $launcher @"
@echo off
chcp 936 >nul
title Dev-C++ 智能版启动器
set DEVAI_HOME=$DevAIHomeDir
set PORT=47901
if exist "%DEVAI_HOME%\port.txt" set /p PORT=<"%DEVAI_HOME%\port.txt"
echo =================================================
echo    DevAI 小助手  正在启动（Dev-C++ + 右侧助手面板）
echo =================================================
if not exist "%DEVAI_HOME%\node_path.txt" goto noagent

rem 1) 先启动 Dev-C++，助手面板一出来就能贴到它右边（已经在跑就不再开一个）
set DEVCPP=$devRoot\DevCpp.exe
if defined DEVCPP tasklist /fi "imagename eq DevCpp.exe" 2>nul | find /i "DevCpp.exe" >nul || start "" "%DEVCPP%"
timeout /t 2 /nobreak >nul

rem 2) 启动助手服务
wscript //nologo "%DEVAI_HOME%\bin\start-agent.vbs"

rem 3) 打开右侧助手面板（服务没起来或窗口没出现时，脚本会自己拉起浏览器窗口）
powershell -NoProfile -ExecutionPolicy Bypass -File "%DEVAI_HOME%\bin\open-panel.ps1" -Port %PORT%

rem 4) 清空上一轮的分析记录，面板从空白开始
powershell -NoProfile -Command "try{Invoke-WebRequest -UseBasicParsing -Method Post -Uri 'http://127.0.0.1:%PORT%/api/reset' -Body '{}' -ContentType 'application/json' -TimeoutSec 3 | Out-Null}catch{}" >nul 2>&1

rem 5) 玻璃浮层与窗口吸附（已经在跑就不重复开）
tasklist /fi "imagename eq devai_glass.exe" 2>nul | find /i "devai_glass.exe" >nul || start "" "%DEVAI_HOME%\bin\devai_glass.exe"
tasklist /fi "imagename eq devai_dock.exe" 2>nul | find /i "devai_dock.exe" >nul || start "" "%DEVAI_HOME%\bin\devai_dock.exe"

echo [DevAI] 已全部启动：Dev-C++ 在左，助手面板在右。本窗口 3 秒后自动关闭。
timeout /t 3 /nobreak >nul
exit

:noagent
echo [DevAI] 未检测到 Node.js，AI 讲解功能不可用，仅启动 Dev-C++ 和玻璃浮层。
start "" "$devRoot\DevCpp.exe"
tasklist /fi "imagename eq devai_glass.exe" 2>nul | find /i "devai_glass.exe" >nul || start "" "%DEVAI_HOME%\bin\devai_glass.exe"
timeout /t 4 /nobreak >nul
exit
"@
if (-not (Test-Path $plainLauncher)) { Copy-Item $launcher $plainLauncher -Force }
$launchers += $launcher
}
$launcher = $launchers[0]

$panel = Join-Path $DevAIHomeDir '打开助手面板.bat'
Write-GbkFile $panel @"
@echo off
chcp 936 >nul
title DevAI 助手面板
set DEVAI_HOME=$DevAIHomeDir
if not exist "%DEVAI_HOME%\node_path.txt" (
  echo [DevAI] 还没装 Node.js，无法启动助手。
  pause
  exit
)
set PORT=47901
if exist "%DEVAI_HOME%\port.txt" set /p PORT=<"%DEVAI_HOME%\port.txt"
wscript //nologo "%DEVAI_HOME%\bin\start-agent.vbs"
powershell -NoProfile -ExecutionPolicy Bypass -File "%DEVAI_HOME%\bin\open-panel.ps1" -Port %PORT%
echo [DevAI] 面板已打开，看屏幕右上角。
timeout /t 3 /nobreak >nul
exit
"@

$agentVbs = Join-Path $DevAIHomeDir 'bin\start-agent.vbs'
$vbs = @'
Dim fso, node, srv, sh
Set fso = CreateObject("Scripting.FileSystemObject")
node = Trim(fso.OpenTextFile("__NODE__", 1).ReadAll)
srv = "__SRV__"
Set sh = CreateObject("WScript.Shell")
sh.Run """" & node & """ """ & srv & """", 0, False
'@
$vbs = $vbs.Replace('__NODE__', (Join-Path $DevAIHomeDir 'node_path.txt')).Replace('__SRV__', (Join-Path $DevAIHomeDir 'server\server.js'))
[System.IO.File]::WriteAllText($agentVbs, $vbs, [System.Text.Encoding]::ASCII)

# 开始菜单快捷方式（不往桌面乱丢文件）
$plainLnk = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Dev-C++ 智能版.lnk'
foreach ($t in $targets) {
    try {
        $ws = New-Object -ComObject WScript.Shell
        $sm = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
        $lnk = Join-Path $sm ("Dev-C++ 智能版 ({0}).lnk" -f $t.Label)
        $s = $ws.CreateShortcut($lnk)
        $s.TargetPath = (Join-Path $DevAIHomeDir ("启动 Dev-C++ 智能版 ({0}).bat" -f $t.Label))
        $s.WorkingDirectory = $DevAIHomeDir
        $s.IconLocation = (Join-Path $t.Root 'DevCpp.exe') + ',0'
        $s.Description = '启动 Dev-C++ 并同时开启 DevAI 报错讲解助手'
        $s.Save()
        if (-not (Test-Path $plainLnk)) { Copy-Item $lnk $plainLnk -Force }
        Write-Ok "开始菜单快捷方式：$lnk"
    } catch { Write-Warn2 "创建快捷方式失败（不影响使用）：$($_.Exception.Message)" }
}

# 安装记录：按 Dev-C++ 目录分别记，支持多个版本共存
$installs = @()
if (Test-Path $StateFile) {
    try {
        $old = Get-Content $StateFile -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($old.installs) { $installs = @($old.installs) }
        elseif ($old.devCppRoot) {
            $installs = @([pscustomobject]@{ devCppRoot = $old.devCppRoot; mingwBin = $old.mingwBin; version = $old.version })
        }
    } catch {}
}
foreach ($t in $targets) {
    $installs = @($installs | Where-Object { $_.devCppRoot -ne $t.Root })
    $tv = (Get-Item (Join-Path $t.Root 'DevCpp.exe')).VersionInfo.FileVersion
    $installs += [pscustomobject]@{
        devCppRoot = $t.Root
        mingwBin   = $t.Bin
        version    = $tv
        label      = $t.Label
    }
}
$state = @{
    devCppRoot  = $DevCppRoot
    mingwBin    = $MingwBin
    home        = $DevAIHomeDir
    installedAt = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
    version     = $verText
    installs    = @($installs)
} | ConvertTo-Json -Depth 4
[System.IO.File]::WriteAllText($StateFile, $state, $Utf8)

# ------------------------------------------------------------------ 6. 完成
Write-Step '步骤 6/6  完成'
Write-Host ''
Write-Host '  安装成功！接下来只需要两步：' -ForegroundColor White
Write-Host '  1) 打开  ' -NoNewline; Write-Host "$launcher" -ForegroundColor Yellow
Write-Host '  2) 第一次打开助手面板时，点右上角齿轮，把 DeepSeek 的 API Key 填进去' -ForegroundColor White
Write-Host ''
Write-Host '  之后在 Dev-C++ 里照旧按 F9 / F11，报错、崩溃会自动弹讲解。' -ForegroundColor Gray
Write-Host '  不想用了就双击  卸载 DevAI.bat  ，编译器和配置会原样还原。' -ForegroundColor Gray
Write-Host ''
Write-Host "  运行目录：$DevAIHomeDir" -ForegroundColor DarkGray
Write-Host "  备份文件：$(Join-Path $DevAIHomeDir 'backup')" -ForegroundColor DarkGray
Write-Host '=================================================' -ForegroundColor DarkCyan
if (-not $SkipWait) {
    Write-Host '  本窗口 15 秒后自动关闭 ...' -ForegroundColor DarkGray
    Start-Sleep -Seconds 15
}
