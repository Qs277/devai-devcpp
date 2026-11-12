# 生成 DevAI 图标：多尺寸 ICO + 一张 512 PNG
# 风格：正方形硬边，左青 / 右淡红，中间小机器人半身像（无圆角）
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-icon.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$outIco = Join-Path $root 'res\devai.ico'
$outPng = Join-Path $root 'docs\icon.png'
New-Item -ItemType Directory -Force -Path (Split-Path $outIco) | Out-Null

function Draw-Icon([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    $u = $size / 256.0

    $cyan = [System.Drawing.Color]::FromArgb(255, 14, 154, 167)
    $rose = [System.Drawing.Color]::FromArgb(255, 224, 108, 99)
    $ink  = [System.Drawing.Color]::FromArgb(255, 26, 31, 35)
    $white = [System.Drawing.Color]::FromArgb(255, 255, 255, 255)

    # 整块方形底：左青右淡红
    $g.FillRectangle((New-Object System.Drawing.SolidBrush($cyan)), 0, 0, [int][Math]::Ceiling(128 * $u), $size)
    $g.FillRectangle((New-Object System.Drawing.SolidBrush($rose)), [int][Math]::Ceiling(128 * $u), 0,
                     $size - [int][Math]::Ceiling(128 * $u), $size)

    # 极细中缝，强调分割
    $gapW = [Math]::Max(1.0, 3 * $u)
    $g.FillRectangle((New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(70, 255, 255, 255))),
                     [float](128 * $u - $gapW / 2), [float]0, [float]$gapW, [float]$size)

    # 描边笔
    $penW = [Math]::Max(1.0, 9 * $u)
    $pen = New-Object System.Drawing.Pen($ink, [float]$penW)
    $pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Miter
    $whiteBrush = New-Object System.Drawing.SolidBrush($white)
    $inkBrush = New-Object System.Drawing.SolidBrush($ink)

    # 天线
    $aw = [Math]::Max(1.0, 8 * $u)
    $antPen = New-Object System.Drawing.Pen($ink, [float]$aw)
    $g.DrawLine($antPen, [float](128 * $u), [float](54 * $u), [float](128 * $u), [float](26 * $u))
    $g.DrawRectangle($antPen, [float](120 * $u), [float](14 * $u), [float](16 * $u), [float](16 * $u))

    # 头（方形）
    $hx = 74 * $u; $hy = 54 * $u; $hw = 108 * $u; $hh = 92 * $u
    $g.FillRectangle($whiteBrush, [float]$hx, [float]$hy, [float]$hw, [float]$hh)
    $g.DrawRectangle($pen, [float]$hx, [float]$hy, [float]$hw, [float]$hh)

    # 眼睛（方形）
    $ew = [Math]::Max(1.0, 18 * $u)
    $eh = [Math]::Max(1.0, 22 * $u)
    $eyy = 88 * $u
    $g.FillRectangle($inkBrush, [float](102 * $u), [float]$eyy, [float]$ew, [float]$eh)
    $g.FillRectangle($inkBrush, [float](136 * $u), [float]$eyy, [float]$ew, [float]$eh)

    # 嘴（方条）
    $g.FillRectangle($inkBrush, [float](112 * $u), [float](122 * $u), [float](32 * $u), [float](8 * $u))

    # 身体 / 肩（梯形，方形硬边）
    $body = New-Object System.Drawing.Drawing2D.GraphicsPath
    $body.AddPolygon(@(
        (New-Object System.Drawing.PointF([float](100 * $u), [float](146 * $u))),
        (New-Object System.Drawing.PointF([float](156 * $u), [float](146 * $u))),
        (New-Object System.Drawing.PointF([float](184 * $u), [float](242 * $u))),
        (New-Object System.Drawing.PointF([float](72 * $u), [float](242 * $u)))
    ))
    $g.FillPath($whiteBrush, $body)
    $g.DrawPath($pen, $body)

    # 胸口小方块：左青右淡红，呼应底色
    if ($size -ge 24) {
        $g.FillRectangle((New-Object System.Drawing.SolidBrush($cyan)),
                         [float](118 * $u), [float](182 * $u), [float](10 * $u), [float](32 * $u))
        $g.FillRectangle((New-Object System.Drawing.SolidBrush($rose)),
                         [float](128 * $u), [float](182 * $u), [float](10 * $u), [float](32 * $u))
    }

    $g.Dispose()
    return $bmp
}

# --- 多尺寸 PNG ---
$sizes = @(16, 24, 32, 48, 64, 128, 256)
$pngs = @()
foreach ($s in $sizes) {
    $bmp = Draw-Icon $s
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs += , @{ Size = $s; Bytes = $ms.ToArray() }
    $ms.Dispose()
    $bmp.Dispose()
}

# --- 写 ICO ---
$fs = [System.IO.File]::Create($outIco)
$bw = New-Object System.IO.BinaryWriter($fs)
$bw.Write([UInt16]0)              # reserved
$bw.Write([UInt16]1)              # type = icon
$bw.Write([UInt16]$pngs.Count)
$offset = 6 + 16 * $pngs.Count
foreach ($p in $pngs) {
    $w = if ($p.Size -ge 256) { 0 } else { $p.Size }
    $bw.Write([Byte]$w)
    $bw.Write([Byte]$w)
    $bw.Write([Byte]0)            # color count
    $bw.Write([Byte]0)            # reserved
    $bw.Write([UInt16]1)          # planes
    $bw.Write([UInt16]32)         # bpp
    $bw.Write([UInt32]$p.Bytes.Length)
    $bw.Write([UInt32]$offset)
    $offset += $p.Bytes.Length
}
foreach ($p in $pngs) { $bw.Write($p.Bytes) }
$bw.Flush(); $bw.Close(); $fs.Close()

# --- 512 PNG（README / 宣传用）---
$bmp = Draw-Icon 512
$bmp.Save($outPng, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

Write-Host ("icon -> {0} ({1} bytes)" -f $outIco, (Get-Item $outIco).Length)
Write-Host ("png  -> {0}" -f $outPng)
