# DevAI 安装器：自动检测本机 Dev-C++，接入 AI 编译/运行助手
# 用法：右键 安装 DevAI.bat → 以管理员身份运行（普通权限一般也够）
param(
    [string]$DevAIHome = "",
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

function Get-DevCppRoot {
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
                     "$env:LOCALAPPDATA\Programs\Dev-Cpp")) {
        [void]$cands.Add($c)
    }

    foreach ($c in $cands) {
        if (-not $c) { continue }
        $exe = Join-Path $c 'DevCpp.exe'
        if (Test-Path $exe) { return (Resolve-Path $c).Path }
    }
    return $null
}

$DevCppRoot = Get-DevCppRoot
if (-not $DevCppRoot) {
    Write-Warn2 '没有在注册表/常见目录里找到 Dev-C++。'
    $manual = Read-Host '请输入 Dev-C++ 安装目录（例如 D:\dev\Dev-Cpp，直接回车放弃）'
    if ($manual -and (Test-Path (Join-Path $manual 'DevCpp.exe'))) { $DevCppRoot = (Resolve-Path $manual).Path }
}
if (-not $DevCppRoot) { Write-Host '[DevAI] 未能定位 Dev-C++，安装中止。' -ForegroundColor Red; if(-not $SkipWait){ Start-Sleep 15 }; exit 1 }

$MingwBin = Join-Path $DevCppRoot 'MinGW64\bin'
if (-not (Test-Path $MingwBin)) {
    Write-Host "[DevAI] 找到了 $DevCppRoot，但里面没有 MinGW64\bin，可能是精简版，安装中止。" -ForegroundColor Red
    if (-not $SkipWait) { Start-Sleep 15 }
    exit 1
}

$ver = (Get-Item (Join-Path $DevCppRoot 'DevCpp.exe')).VersionInfo
$verText = if ($ver.FileVersion) { $ver.FileVersion } else { '未知版本' }
Write-Ok "Dev-C++ 安装目录：$DevCppRoot"
Write-Ok "Dev-C++ 版本：$verText"
Write-Ok "编译器目录：$MingwBin"

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
Write-Ok "运行目录：$DevAIHomeDir（含窗口嵌入组件 devai_dock.exe）"

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

$gcc = Join-Path $MingwBin 'gcc.exe'
$gpp = Join-Path $MingwBin 'g++.exe'
$gccReal = Join-Path $MingwBin 'gcc-real.exe'
$gppReal = Join-Path $MingwBin 'g++-real.exe'

# 真编译器改名（只做一次，之后重复安装不会重复改名）
if ((Test-Path $gcc) -and (-not (Test-Path $gccReal))) { Rename-Item $gcc 'gcc-real.exe' }
if ((Test-Path $gpp) -and (-not (Test-Path $gppReal))) { Rename-Item $gpp 'g++-real.exe' }

# 放入拦截器，占用 gcc/g++ 名字
Copy-Item (Join-Path $PluginRoot 'bin\devai_gcc.exe') $gcc -Force
Copy-Item (Join-Path $PluginRoot 'bin\devai_gpp.exe') $gpp -Force
Write-Ok 'gcc.exe / g++.exe 已接入 DevAI 拦截层'

# 把安装目录写成标记文件：用户直接双击 Dev-C++（不走启动器、没有 DEVAI_HOME）时拦截器也能找到 DevAI
$gbk = [System.Text.Encoding]::GetEncoding(936)
[System.IO.File]::WriteAllText((Join-Path $MingwBin 'devai-home.txt'), $DevAIHomeDir, $gbk)
[System.IO.File]::WriteAllText((Join-Path $DevCppRoot 'devai-home.txt'), $DevAIHomeDir, $gbk)
Write-Ok "已记录安装目录：$DevAIHomeDir"

# 运行拦截：替换 ConsolePauser.exe（原文件备份为 ConsolePauser.orig.exe）
$cp = Join-Path $DevCppRoot 'ConsolePauser.exe'
$cpOrig = Join-Path $DevCppRoot 'ConsolePauser.orig.exe'
if ((Test-Path $cp) -and (-not (Test-Path $cpOrig))) { Copy-Item $cp $cpOrig -Force }
Copy-Item (Join-Path $PluginRoot 'bin\devai_console.exe') $cp -Force
Write-Ok 'ConsolePauser.exe 已接入运行监控（崩溃退出码可被捕获）'

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

$launcher = Join-Path $DevAIHomeDir '启动 Dev-C++ 智能版.bat'
Write-GbkFile $launcher @"
@echo off
chcp 936 >nul
title Dev-C++ 智能版启动器
set DEVAI_HOME=$DevAIHomeDir
echo =================================================
echo    DevAI 小助手  正在启动 ...
echo =================================================
if not exist "%DEVAI_HOME%\node_path.txt" goto noagent
wscript //nologo "%DEVAI_HOME%\bin\start-agent.vbs"
timeout /t 2 /nobreak >nul
echo [DevAI] 助手已就绪，正在打开 Dev-C++ ...
start "" "$DevCppRoot\DevCpp.exe"
echo [DevAI] 祝编译顺利！本窗口 5 秒后自动关闭。
timeout /t 5 /nobreak >nul
exit
:noagent
echo [DevAI] 未检测到 Node.js，AI 讲解功能不可用，仅启动 Dev-C++。
start "" "$DevCppRoot\DevCpp.exe"
timeout /t 5 /nobreak >nul
exit
"@

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
for /l %%i in (1,1,15) do (
  powershell -NoProfile -Command "try{(Invoke-WebRequest -UseBasicParsing -Method Post -Uri 'http://127.0.0.1:%PORT%/api/show-panel' -Body '{}' -ContentType 'application/json' -TimeoutSec 2).StatusCode}catch{exit 1}" >nul 2>&1 && goto opened
  timeout /t 1 /nobreak >nul
)
echo [DevAI] 助手服务还没起来，稍等几秒再点一次。
timeout /t 4 /nobreak >nul
exit
:opened
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
try {
    $ws = New-Object -ComObject WScript.Shell
    $sm = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
    $lnk = Join-Path $sm 'Dev-C++ 智能版.lnk'
    $s = $ws.CreateShortcut($lnk)
    $s.TargetPath = $launcher
    $s.WorkingDirectory = $DevAIHomeDir
    $s.IconLocation = (Join-Path $DevCppRoot 'DevCpp.exe') + ',0'
    $s.Description = '启动 Dev-C++ 并同时开启 DevAI 报错讲解助手'
    $s.Save()
    Write-Ok "开始菜单快捷方式：$lnk"
} catch { Write-Warn2 "创建快捷方式失败（不影响使用）：$($_.Exception.Message)" }

$state = @{
    devCppRoot = $DevCppRoot
    mingwBin = $MingwBin
    home = $DevAIHomeDir
    installedAt = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')
    version = $verText
} | ConvertTo-Json
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
