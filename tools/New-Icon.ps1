[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$taskAssets = Join-Path (Split-Path $PSScriptRoot -Parent) 'assets'
function New-TaskRoundedRectangle([single]$X,[single]$Y,[single]$Width,[single]$Height,[single]$Radius) {
    $taskPath = New-Object System.Drawing.Drawing2D.GraphicsPath
    $taskDiameter = $Radius * 2
    $taskPath.AddArc($X,$Y,$taskDiameter,$taskDiameter,180,90)
    $taskPath.AddArc(($X+$Width-$taskDiameter),$Y,$taskDiameter,$taskDiameter,270,90)
    $taskPath.AddArc(($X+$Width-$taskDiameter),($Y+$Height-$taskDiameter),$taskDiameter,$taskDiameter,0,90)
    $taskPath.AddArc($X,($Y+$Height-$taskDiameter),$taskDiameter,$taskDiameter,90,90)
    $taskPath.CloseFigure()
    return ,$taskPath
}
$taskFrames = @()
foreach ($taskSize in @(16,24,32,48,64,128,256)) {
    $taskBitmap = New-Object System.Drawing.Bitmap($taskSize,$taskSize)
    $taskGraphics = [System.Drawing.Graphics]::FromImage($taskBitmap)
    $taskGraphics.SmoothingMode = 'AntiAlias'
    $taskGraphics.ScaleTransform(($taskSize/256.0),($taskSize/256.0))
    $taskBackground = New-TaskRoundedRectangle 8 8 240 240 54
    $taskBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush([System.Drawing.Point]::new(0,0),[System.Drawing.Point]::new(256,256),[System.Drawing.ColorTranslator]::FromHtml('#7165ee'),[System.Drawing.ColorTranslator]::FromHtml('#393392'))
    $taskGraphics.FillPath($taskBrush,$taskBackground)
    $taskBubble = New-Object System.Drawing.Drawing2D.GraphicsPath
    $taskBubble.AddLines([System.Drawing.PointF[]]@([System.Drawing.PointF]::new(65,57),[System.Drawing.PointF]::new(191,57)))
    $taskBubble.AddArc(174,57,34,34,270,90)
    $taskBubble.AddLine(208,74,208,165)
    $taskBubble.AddArc(174,148,34,34,0,90)
    $taskBubble.AddLines([System.Drawing.PointF[]]@([System.Drawing.PointF]::new(191,182),[System.Drawing.PointF]::new(127,182),[System.Drawing.PointF]::new(95,208),[System.Drawing.PointF]::new(95,182),[System.Drawing.PointF]::new(65,182)))
    $taskBubble.AddArc(48,148,34,34,90,90)
    $taskBubble.AddLine(48,165,48,74)
    $taskBubble.AddArc(48,57,34,34,180,90)
    $taskBubble.CloseFigure()
    $taskPen = New-Object System.Drawing.Pen([System.Drawing.Color]::White,10)
    $taskPen.LineJoin = 'Round'
    $taskGraphics.DrawPath($taskPen,$taskBubble)
    $taskMint = New-Object System.Drawing.SolidBrush([System.Drawing.ColorTranslator]::FromHtml('#78efdc'))
    foreach ($taskBar in @(@(76,119,21,36),@(117,83,21,72),@(158,101,21,54))) {
        $taskBarPath = New-TaskRoundedRectangle $taskBar[0] $taskBar[1] $taskBar[2] $taskBar[3] 5
        $taskGraphics.FillPath($(if ($taskBar[0] -eq 117) { [System.Drawing.Brushes]::White } else { $taskMint }),$taskBarPath)
        $taskBarPath.Dispose()
    }
    $taskStream = New-Object IO.MemoryStream
    $taskBitmap.Save($taskStream,[System.Drawing.Imaging.ImageFormat]::Png)
    $taskFrames += [pscustomobject]@{Size=$taskSize;Bytes=$taskStream.ToArray()}
    if ($taskSize -eq 256) { [IO.File]::WriteAllBytes((Join-Path $taskAssets 'session-stats.png'),$taskStream.ToArray()) }
    $taskStream.Dispose(); $taskGraphics.Dispose(); $taskBitmap.Dispose()
    $taskBackground.Dispose(); $taskBubble.Dispose(); $taskBrush.Dispose(); $taskPen.Dispose(); $taskMint.Dispose()
}
$taskOutput = [IO.File]::Create((Join-Path $taskAssets 'session-stats.ico'))
$taskWriter = New-Object IO.BinaryWriter($taskOutput)
try {
    $taskWriter.Write([uint16]0); $taskWriter.Write([uint16]1); $taskWriter.Write([uint16]$taskFrames.Count)
    $taskOffset = 6 + 16 * $taskFrames.Count
    foreach ($taskFrame in $taskFrames) {
        $taskDimension = if ($taskFrame.Size -eq 256) { 0 } else { $taskFrame.Size }
        $taskWriter.Write([byte]$taskDimension); $taskWriter.Write([byte]$taskDimension)
        $taskWriter.Write([byte]0); $taskWriter.Write([byte]0)
        $taskWriter.Write([uint16]1); $taskWriter.Write([uint16]32)
        $taskWriter.Write([uint32]$taskFrame.Bytes.Length); $taskWriter.Write([uint32]$taskOffset)
        $taskOffset += $taskFrame.Bytes.Length
    }
    foreach ($taskFrame in $taskFrames) { $taskWriter.Write([byte[]]$taskFrame.Bytes) }
} finally { $taskWriter.Dispose(); $taskOutput.Dispose() }
Write-Output (Join-Path $taskAssets 'session-stats.ico')
