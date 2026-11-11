# DevAI 启动器：自动扫描本机所有 Dev-C++，弹窗选一个，然后连助手一起拉起
# 由「启动 Dev-C++ 智能版.bat」调用；也可以直接 powershell -File 本文件
param([switch]$NoPick)

$ErrorActionPreference = 'SilentlyContinue'

# bin\launcher.ps1 -> 上一级就是 DevAI 安装目录
$DevAIHomeDir = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $DevAIHomeDir 'install-state.json'))) { $DevAIHomeDir = $PSScriptRoot }
$env:DEVAI_HOME = $DevAIHomeDir

# ------------------------------------------------------------------ 扫描版本
$roots = New-Object System.Collections.ArrayList
function Add-Root([string]$r) {
    if (-not $r) { return }
    $exe = Join-Path $r 'DevCpp.exe'
    if (-not (Test-Path $exe)) { return }
    foreach ($x in $roots) { if ($x.Root -ieq $r) { return } }
    $ver = ''
    try { $ver = (Get-Item $exe).VersionInfo.FileVersion } catch {}
    if (-not $ver) { $ver = '版本未知' }
    [void]$roots.Add([pscustomobject]@{ Root = $r; Exe = $exe; Version = ($ver.Trim()) })
}

# 1) 安装记录里的（装过智能版的）
$stateFile = Join-Path $DevAIHomeDir 'install-state.json'
if (Test-Path $stateFile) {
    try {
        $st = Get-Content $stateFile -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($st.installs) { foreach ($i in $st.installs) { Add-Root $i.devCppRoot } }
        elseif ($st.devCppRoot) { Add-Root $st.devCppRoot }
    } catch {}
}

# 2) 常见安装位置 + 各盘根目录下一层的 Dev-Cpp（只扫两层，不做全盘搜索）
$probe = @(
    (Join-Path $env:ProgramFiles 'Dev-Cpp'),
    (Join-Path ${env:ProgramFiles(x86)} 'Dev-Cpp'),
    (Join-Path $env:LOCALAPPDATA 'Programs\Dev-Cpp'),
    'C:\Dev-Cpp', 'D:\Dev-Cpp', 'E:\Dev-Cpp'
)
foreach ($drv in @('C:\', 'D:\', 'E:\')) {
    if (-not (Test-Path $drv)) { continue }
    Get-ChildItem $drv -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        $probe += (Join-Path $_.FullName 'Dev-Cpp')
    }
}
foreach ($p in $probe) { Add-Root $p }

if ($roots.Count -eq 0) {
    Add-Type -AssemblyName System.Windows.Forms
    [void][System.Windows.Forms.MessageBox]::Show(
        "没有找到 Dev-C++。`n`n请先安装 Dev-C++，或重新运行 安装 DevAI.bat。",
        'DevAI 小助手', 'OK', 'Warning')
    exit 1
}

# ------------------------------------------------------------------ 选一个
$pick = $roots[0]
if ($roots.Count -gt 1 -and -not $NoPick) {
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing

    $form = New-Object System.Windows.Forms.Form
    $form.Text = '选择要打开的 Dev-C++'
    $form.Size = New-Object System.Drawing.Size(520, 300)
    $form.StartPosition = 'CenterScreen'
    $form.FormBorderStyle = 'FixedDialog'
    $form.MaximizeBox = $false
    $form.MinimizeBox = $false
    $form.TopMost = $true
    $form.Font = New-Object System.Drawing.Font('Microsoft YaHei UI', 9)

    $tip = New-Object System.Windows.Forms.Label
    $tip.Text = '检测到本机有多个 Dev-C++，选一个打开：'
    $tip.AutoSize = $true
    $tip.Location = New-Object System.Drawing.Point(16, 14)
    $form.Controls.Add($tip)

    $list = New-Object System.Windows.Forms.ListBox
    $list.Location = New-Object System.Drawing.Point(16, 42)
    $list.Size = New-Object System.Drawing.Size(472, 170)
    $list.IntegralHeight = $false
    foreach ($r in $roots) { [void]$list.Items.Add(('{0}    {1}' -f $r.Version, $r.Root)) }
    $list.SelectedIndex = 0
    $form.Controls.Add($list)

    $ok = New-Object System.Windows.Forms.Button
    $ok.Text = '打开'
    $ok.Size = New-Object System.Drawing.Size(96, 30)
    $ok.Location = New-Object System.Drawing.Point(284, 224)
    $ok.DialogResult = 'OK'
    $form.Controls.Add($ok)

    $cancel = New-Object System.Windows.Forms.Button
    $cancel.Text = '取消'
    $cancel.Size = New-Object System.Drawing.Size(96, 30)
    $cancel.Location = New-Object System.Drawing.Point(392, 224)
    $cancel.DialogResult = 'Cancel'
    $form.Controls.Add($cancel)

    $form.AcceptButton = $ok
    $form.CancelButton = $cancel
    $list.Add_DoubleClick({ $form.DialogResult = 'OK'; $form.Close() })

    if ($form.ShowDialog() -ne 'OK') { exit 0 }
    if ($list.SelectedIndex -ge 0) { $pick = $roots[$list.SelectedIndex] }
}

# ------------------------------------------------------------------ 启动
$port = 47901
$portFile = Join-Path $DevAIHomeDir 'port.txt'
if (Test-Path $portFile) {
    $p = (Get-Content $portFile -Raw).Trim()
    if ($p) { $port = $p }
}

# 1) Dev-C++ 本身（已经在跑就不再开一个）
$running = Get-Process DevCpp -ErrorAction SilentlyContinue
if (-not $running) {
    Start-Process -FilePath $pick.Exe -WorkingDirectory $pick.Root
    Start-Sleep -Milliseconds 1500
}

$hasNode = Test-Path (Join-Path $DevAIHomeDir 'node_path.txt')
if ($hasNode) {
    # 2) 助手服务
    $vbs = Join-Path $DevAIHomeDir 'bin\start-agent.vbs'
    if (Test-Path $vbs) { Start-Process -FilePath 'wscript.exe' -ArgumentList ('//nologo "' + $vbs + '"') -WindowStyle Hidden }

    # 3) 右侧助手面板
    $openPanel = Join-Path $DevAIHomeDir 'bin\open-panel.ps1'
    if (Test-Path $openPanel) {
        Start-Process -FilePath 'powershell.exe' -ArgumentList ('-NoProfile -ExecutionPolicy Bypass -File "' + $openPanel + '" -Port ' + $port) -WindowStyle Hidden
    }

    # 4) 清掉上一轮分析
    try {
        Invoke-WebRequest -UseBasicParsing -Method Post -Uri ("http://127.0.0.1:{0}/api/reset" -f $port) -Body '{}' -ContentType 'application/json' -TimeoutSec 3 | Out-Null
    } catch {}
}

# 5) 玻璃浮层与吸附（已经在跑就不重复开）
foreach ($proc in @('devai_glass', 'devai_dock')) {
    $exe = Join-Path $DevAIHomeDir ('bin\' + $proc + '.exe')
    if (-not (Test-Path $exe)) { continue }
    if (-not (Get-Process $proc -ErrorAction SilentlyContinue)) {
        Start-Process -FilePath $exe -WorkingDirectory $DevAIHomeDir
    }
}
exit 0
