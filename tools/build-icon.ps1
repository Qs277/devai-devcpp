# 生成 DevAI 图标：多尺寸 ICO + 一张 512 PNG
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File tools\build-icon.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$outIco = Join-Path $root 'res\devai.ico'
$outPng = Join-Path $root 'docs\icon.png'
New-Item -ItemType Directory -Force -Path (Split-Path $outIco) | Out-Null

function New-RoundPath([double]$x, [double]$y, [double]$w, [double]$h, [double]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function New-SparklePath([double]$cx, [double]$cy, [double]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $k = $r * 0.26
    $pts = @(
        (New-Object System.Drawing.PointF([float]$cx, [float]($cy - $r))),
        (New-Object System.Drawing.PointF([float]($cx + $k), [float]($cy - $k))),
        (New-Object System.Drawing.PointF([float]($cx + $r), [float]$cy)),
        (New-Object System.Drawing.PointF([float]($cx + $k), [float]($cy + $k))),
        (New-Object System.Drawing.PointF([float]$cx, [float]($cy + $r))),
        (New-Object System.Drawing.PointF([float]($cx - $k), [float]($cy + $k))),
        (New-Object System.Drawing.PointF([float]($cx - $r), [float]$cy)),
        (New-Object System.Drawing.PointF([float]($cx - $k), [float]($cy - $k)))
    )
    $p.AddPolygon($pts)
    return $p
}

function Draw-Icon([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    $u = $size / 256.0

    # 背景：靛蓝 → 紫罗兰渐变圆角方块
    $bg = New-RoundPath (2 * $u) (2 * $u) (252 * $u) (252 * $u) (58 * $u)
    $rect = New-Object System.Drawing.RectangleF([float](2 * $u), [float](2 * $u), [float](252 * $u), [float](252 * $u))
    $c1 = [System.Drawing.Color]::FromArgb(255, 84, 132, 255)
    $c2 = [System.Drawing.Color]::FromArgb(255, 138, 92, 246)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $c1, $c2, 130.0)
    $g.FillPath($brush, $bg)

    # 顶部柔光
    $glowPath = New-RoundPath (2 * $u) (2 * $u) (252 * $u) (150 * $u) (58 * $u)
    $glow = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.RectangleF([float](2 * $u), [float](2 * $u), [float](252 * $u), [float](150 * $u))),
        [System.Drawing.Color]::FromArgb(46, 255, 255, 255),
        [System.Drawing.Color]::FromArgb(0, 255, 255, 255),
        90.0)
    $g.FillPath($glow, $glowPath)

    # 内描边
    $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(38, 255, 255, 255), [float](3 * $u))
    $g.DrawPath($pen, $bg)

    # 白色对话气泡
    $bubble = New-RoundPath (46 * $u) (52 * $u) (164 * $u) (126 * $u) (44 * $u)
    $tail = New-Object System.Drawing.Drawing2D.GraphicsPath
    $tail.AddPolygon(@(
        (New-Object System.Drawing.PointF([float](84 * $u), [float](170 * $u))),
        (New-Object System.Drawing.PointF([float](120 * $u), [float](170 * $u))),
        (New-Object System.Drawing.PointF([float](92 * $u), [float](204 * $u)))
    ))
    $white = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 255, 255, 255))
    $g.FillPath($white, $bubble)
    $g.FillPath($white, $tail)

    # 眼睛
    $eye = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 48, 34, 96))
    $ew = [Math]::Max(2.0, 22 * $u)
    $eh = [Math]::Max(2.0, 26 * $u)
    $ey = 104 * $u
    $g.FillEllipse($eye, [float](102 * $u), [float]$ey, [float]$ew, [float]$eh)
    $g.FillEllipse($eye, [float](134 * $u), [float]$ey, [float]$ew, [float]$eh)
    if ($size -ge 24) {
        $eyeGl = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(210, 255, 255, 255))
        $gw = [Math]::Max(1.0, 7 * $u)
        $g.FillEllipse($eyeGl, [float](105 * $u), [float](109 * $u), [float]$gw, [float](7 * $u))
        $g.FillEllipse($eyeGl, [float](137 * $u), [float](109 * $u), [float]$gw, [float](7 * $u))
    }

    # 气泡里的代码尖括号
    if ($size -ge 32) {
        $cp = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 126, 108, 240), [float]([Math]::Max(1.6, 8 * $u)))
        $cp.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
        $cp.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
        $g.DrawLines($cp, @(
            (New-Object System.Drawing.PointF([float](84 * $u), [float](128 * $u))),
            (New-Object System.Drawing.PointF([float](70 * $u), [float](144 * $u))),
            (New-Object System.Drawing.PointF([float](84 * $u), [float](160 * $u)))
        ))
        $g.DrawLines($cp, @(
            (New-Object System.Drawing.PointF([float](172 * $u), [float](128 * $u))),
            (New-Object System.Drawing.PointF([float](186 * $u), [float](144 * $u))),
            (New-Object System.Drawing.PointF([float](172 * $u), [float](160 * $u)))
        ))
    }

    # 右上角小星星
    if ($size -ge 24) {
        $sp = New-SparklePath (214 * $u) (42 * $u) (18 * $u)
        $sb = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(240, 255, 255, 255))
        $g.FillPath($sb, $sp)
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
