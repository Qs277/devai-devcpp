# 构建 DevAI 单文件版（x64 / x86）
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-release.ps1
param([switch]$SkipX86)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$build = Join-Path $root 'build'
$stage = Join-Path $build 'payload'
$dist  = Join-Path $root 'dist'
$resDir = Join-Path $root 'res'

$X64BIN = 'D:\dev\Dev-Cpp\MinGW64\bin'
$x86gcc = Get-ChildItem 'D:\dev\mingw32' -Recurse -Filter gcc.exe -ErrorAction SilentlyContinue |
          Where-Object { $_.Name -eq 'gcc.exe' -and (Test-Path (Join-Path $_.DirectoryName 'windres.exe')) } |
          Select-Object -First 1
$X86BIN = if ($x86gcc) { $x86gcc.DirectoryName } else { $null }

Write-Host "== 1/5 准备 payload ==" -ForegroundColor Cyan
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage, (Join-Path $stage 'bin'), (Join-Path $stage 'server\ui'), $dist | Out-Null

Copy-Item 'E:\node.exe' (Join-Path $stage 'node.exe') -Force
Copy-Item 'bin\devai_console.exe','bin\devai_dock.exe','bin\devai_gcc.exe','bin\devai_glass.exe','bin\devai_gpp.exe','bin\open-panel.ps1' (Join-Path $stage 'bin') -Force
Copy-Item 'server\server.js' (Join-Path $stage 'server') -Force
Copy-Item 'server\ui\index.html' (Join-Path $stage 'server\ui') -Force
Copy-Item 'install.ps1','uninstall.ps1','launcher.ps1' $stage -Force

$payloadZip = Join-Path $build 'payload.zip'
if (Test-Path $payloadZip) { Remove-Item $payloadZip -Force }
Write-Host "   压缩组件（node.exe 比较大，稍等）..."
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $payloadZip -CompressionLevel Optimal
Write-Host ("   payload.zip = {0:N1} MB" -f ((Get-Item $payloadZip).Length / 1MB))

Write-Host "== 2/5 编译资源 ==" -ForegroundColor Cyan
function Build-Res([string]$bin, [string]$out) {
    $env:PATH = "$bin;$env:PATH"
    $gcc = Join-Path $bin 'gcc-real.exe'
    if (-not (Test-Path $gcc)) { $gcc = Join-Path $bin 'gcc.exe' }
    $wr = Join-Path $bin 'windres.exe'
    & $wr "--preprocessor=$($gcc -replace '\\','/') -E -xc-header -DRC_INVOKED" `
          -I $resDir -I $build -O coff -o $out (Join-Path $resDir 'center.rc')
    if ($LASTEXITCODE -ne 0) { throw "windres 失败：$bin" }
}
Build-Res $X64BIN (Join-Path $build 'center_x64.o')

Write-Host "== 3/5 编译 x64 ==" -ForegroundColor Cyan
$cflags = @('-O2','-s','-municode','-mwindows','-static-libgcc')
$libs = @('-lcomctl32','-lwinhttp','-lws2_32','-lshell32','-lole32','-lgdi32',
          '-luser32','-ladvapi32','-lmsimg32','-lcomdlg32','-ldwmapi','-lversion','-lshlwapi')
$x64gcc = Join-Path $X64BIN 'gcc-real.exe'
if (-not (Test-Path $x64gcc)) { $x64gcc = Join-Path $X64BIN 'gcc.exe' }
$outX64 = Join-Path $dist 'DevAI-v1.1.4-x64.exe'
& $x64gcc @cflags -o $outX64 'src\devai_center.c' (Join-Path $build 'center_x64.o') @libs
if ($LASTEXITCODE -ne 0) { throw 'x64 编译失败' }
Write-Host ("   {0} = {1:N1} MB" -f (Split-Path -Leaf $outX64), ((Get-Item $outX64).Length / 1MB))

if (-not $SkipX86 -and $X86BIN) {
    Write-Host "== 4/5 编译 x86（i686）==" -ForegroundColor Cyan
    Build-Res $X86BIN (Join-Path $build 'center_x86.o')
    $outX86 = Join-Path $dist 'DevAI-v1.1.4-x86.exe'
    & (Join-Path $X86BIN 'gcc.exe') @cflags -o $outX86 'src\devai_center.c' (Join-Path $build 'center_x86.o') @libs
    if ($LASTEXITCODE -ne 0) { throw 'x86 编译失败' }
    Write-Host ("   {0} = {1:N1} MB" -f (Split-Path -Leaf $outX86), ((Get-Item $outX86).Length / 1MB))
} elseif (-not $SkipX86) {
    # 没有 i686 工具链时用 MSVC 的 32 位编译器（需要 VS BuildTools + C++ 工具）
    $vs = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools'
    if (-not (Test-Path (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'))) {
        $vs = (Get-ChildItem 'C:\Program Files*\Microsoft Visual Studio\*\BuildTools' -Directory -ErrorAction SilentlyContinue |
               Select-Object -First 1 -ExpandProperty FullName)
    }
    if ($vs -and (Test-Path (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'))) {
        Write-Host "== 4/5 编译 x86（MSVC）==" -ForegroundColor Cyan
        Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
        Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x86 -host_arch=x64' | Out-Null
        Push-Location $root
        rc /nologo /I build /I res /fo (Join-Path $build 'center_x86_msvc.res') (Join-Path $resDir 'center.rc') | Out-Null
        $outX86 = Join-Path $dist 'DevAI-v1.1.4-x86.exe'
        cl /nologo /utf-8 /O2 /MT /D_CRT_SECURE_NO_WARNINGS 'src\devai_center.c' (Join-Path $build 'center_x86_msvc.res') `
           "/Fe:$outX86" /link /SUBSYSTEM:WINDOWS /MACHINE:X86 comctl32.lib winhttp.lib ws2_32.lib shell32.lib `
           ole32.lib gdi32.lib user32.lib advapi32.lib msimg32.lib comdlg32.lib dwmapi.lib version.lib shlwapi.lib | Out-Null
        Pop-Location
        if (-not (Test-Path $outX86)) { throw 'x86 编译失败（MSVC）' }
        Write-Host ("   {0} = {1:N1} MB" -f (Split-Path -Leaf $outX86), ((Get-Item $outX86).Length / 1MB))
    } else {
        Write-Host "== 4/5 跳过 x86（没找到 i686 工具链 / MSVC）==" -ForegroundColor Yellow
    }
} else {
    Write-Host "== 4/5 跳过 x86 ==" -ForegroundColor Yellow
}

Write-Host "== 5/5 完成 ==" -ForegroundColor Green
Get-ChildItem $dist | Select-Object Name, @{ n = 'MB'; e = { [math]::Round($_.Length / 1MB, 1) } } | Format-Table -AutoSize
