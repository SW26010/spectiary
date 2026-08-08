[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Source = '',
    [string]$Output = '',
    [ValidateRange(1, 16)]
    [int]$Supersample = 8
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptRoot '..')).Path
if (-not $Source) {
    $Source = Join-Path $repoRoot 'resources\branding\specforge.svg'
}
if (-not $Output) {
    $Output = Join-Path $repoRoot 'resources\branding\specforge.ico'
}

$resolvedSource = (Resolve-Path -LiteralPath $Source).Path
$resolvedOutput = [IO.Path]::GetFullPath($Output)
$outputDirectory = Split-Path -Parent $resolvedOutput
if (-not (Test-Path -LiteralPath $outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory -Force |
        Out-Null
}

[xml]$svg = Get-Content -Raw -LiteralPath $resolvedSource
$namespaceManager = [Xml.XmlNamespaceManager]::new(
    $svg.NameTable)
$namespaceManager.AddNamespace(
    'svg',
    'http://www.w3.org/2000/svg')

$root = $svg.SelectSingleNode(
    '/svg:svg',
    $namespaceManager)
$background = $svg.SelectSingleNode(
    '/svg:svg/svg:rect',
    $namespaceManager)
$clipRect = $svg.SelectSingleNode(
    '/svg:svg/svg:defs/svg:clipPath/svg:rect',
    $namespaceManager)
$strokeGroup = $svg.SelectSingleNode(
    '/svg:svg/svg:g',
    $namespaceManager)
if ($null -eq $root -or
    $null -eq $background -or
    $null -eq $clipRect -or
    $null -eq $strokeGroup) {
    throw 'The SpecForge SVG does not match the supported icon structure.'
}

$invariantCulture =
    [Globalization.CultureInfo]::InvariantCulture

function Convert-SvgNumber {
    param([Parameter(Mandatory = $true)] [string]$Value)

    return [double]::Parse(
        $Value,
        [Globalization.NumberStyles]::Float,
        $invariantCulture)
}

function Get-SvgAttributeNumber {
    param(
        [Parameter(Mandatory = $true)]
        [Xml.XmlElement]$Element,
        [Parameter(Mandatory = $true)]
        [string]$Name
    )

    return Convert-SvgNumber $Element.GetAttribute($Name)
}

function New-RoundedRectanglePath {
    param(
        [Parameter(Mandatory = $true)]
        [Xml.XmlElement]$Element,
        [Parameter(Mandatory = $true)]
        [double]$Scale
    )

    $x = [single]((Get-SvgAttributeNumber $Element 'x') * $Scale)
    $y = [single]((Get-SvgAttributeNumber $Element 'y') * $Scale)
    $width = [single]((Get-SvgAttributeNumber $Element 'width') * $Scale)
    $height = [single]((Get-SvgAttributeNumber $Element 'height') * $Scale)
    $radius = [single]((Get-SvgAttributeNumber $Element 'rx') * $Scale)
    $diameter = 2.0 * $radius

    $path = [Drawing.Drawing2D.GraphicsPath]::new()
    $path.AddArc($x, $y, $diameter, $diameter, 180, 90)
    $path.AddArc(
        $x + $width - $diameter,
        $y,
        $diameter,
        $diameter,
        270,
        90)
    $path.AddArc(
        $x + $width - $diameter,
        $y + $height - $diameter,
        $diameter,
        $diameter,
        0,
        90)
    $path.AddArc(
        $x,
        $y + $height - $diameter,
        $diameter,
        $diameter,
        90,
        90)
    $path.CloseFigure()
    return $path
}

function Get-SvgPathPoints {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PathData,
        [Parameter(Mandatory = $true)]
        [double]$Scale
    )

    $tokens = @(
        [regex]::Matches(
            $PathData,
            '[ML]|-?\d+(?:\.\d+)?') |
            ForEach-Object Value)
    $points =
        [Collections.Generic.List[Drawing.PointF]]::new()
    for ($index = 0; $index -lt $tokens.Count; $index += 3) {
        if ($tokens[$index] -cnotin @('M', 'L') -or
            $index + 2 -ge $tokens.Count) {
            throw "Unsupported SVG path data: $PathData"
        }
        $points.Add([Drawing.PointF]::new(
                [single]((Convert-SvgNumber $tokens[$index + 1]) * $Scale),
                [single]((Convert-SvgNumber $tokens[$index + 2]) * $Scale)))
    }
    if ($points.Count -lt 2) {
        throw "SVG path requires at least two points: $PathData"
    }
    return ,([Drawing.PointF[]]$points.ToArray())
}

function Draw-SvgStroke {
    param(
        [Parameter(Mandatory = $true)]
        [Drawing.Graphics]$Graphics,
        [Parameter(Mandatory = $true)]
        [Xml.XmlElement]$Element,
        [Parameter(Mandatory = $true)]
        [double]$Scale
    )

    $color = [Drawing.ColorTranslator]::FromHtml(
        $Element.GetAttribute('stroke'))
    $width = [single]((Get-SvgAttributeNumber $Element 'stroke-width') * $Scale)
    $pen = [Drawing.Pen]::new($color, $width)
    try {
        $pen.StartCap = [Drawing.Drawing2D.LineCap]::Round
        $pen.EndCap = [Drawing.Drawing2D.LineCap]::Round
        $pen.LineJoin = [Drawing.Drawing2D.LineJoin]::Round

        if ($Element.LocalName -ceq 'line') {
            $Graphics.DrawLine(
                $pen,
                [single]((Get-SvgAttributeNumber $Element 'x1') * $Scale),
                [single]((Get-SvgAttributeNumber $Element 'y1') * $Scale),
                [single]((Get-SvgAttributeNumber $Element 'x2') * $Scale),
                [single]((Get-SvgAttributeNumber $Element 'y2') * $Scale))
            return
        }
        if ($Element.LocalName -ceq 'path') {
            $points = Get-SvgPathPoints `
                -PathData $Element.GetAttribute('d') `
                -Scale $Scale
            $Graphics.DrawLines($pen, $points)
            return
        }
        throw "Unsupported SVG stroke element: $($Element.LocalName)"
    }
    finally {
        $pen.Dispose()
    }
}

function New-IconPngFrame {
    param(
        [Parameter(Mandatory = $true)]
        [int]$Size
    )

    $viewBox = @(
        $root.GetAttribute('viewBox').Split(
            ' ',
            [StringSplitOptions]::RemoveEmptyEntries) |
            ForEach-Object { Convert-SvgNumber $_ })
    if ($viewBox.Count -ne 4 -or $viewBox[2] -ne $viewBox[3]) {
        throw 'The SpecForge SVG viewBox must be square.'
    }

    $renderSize = $Size * $Supersample
    $scale = $renderSize / $viewBox[2]
    $pixelFormat =
        [Drawing.Imaging.PixelFormat]::Format32bppArgb
    $renderBitmap = [Drawing.Bitmap]::new(
        $renderSize,
        $renderSize,
        $pixelFormat)
    $renderGraphics = [Drawing.Graphics]::FromImage(
        $renderBitmap)
    $finalBitmap = $null
    $finalGraphics = $null
    $pngStream = $null
    try {
        $renderGraphics.Clear([Drawing.Color]::Transparent)
        $renderGraphics.SmoothingMode =
            [Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $renderGraphics.PixelOffsetMode =
            [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $renderGraphics.CompositingQuality =
            [Drawing.Drawing2D.CompositingQuality]::HighQuality

        $backgroundPath = New-RoundedRectanglePath `
            -Element $background `
            -Scale $scale
        $backgroundBrush = [Drawing.SolidBrush]::new(
            [Drawing.ColorTranslator]::FromHtml(
                $background.GetAttribute('fill')))
        try {
            $renderGraphics.FillPath(
                $backgroundBrush,
                $backgroundPath)
        }
        finally {
            $backgroundBrush.Dispose()
            $backgroundPath.Dispose()
        }

        $clipPath = New-RoundedRectanglePath `
            -Element $clipRect `
            -Scale $scale
        try {
            $renderGraphics.SetClip(
                $clipPath,
                [Drawing.Drawing2D.CombineMode]::Replace)
            foreach ($child in $strokeGroup.ChildNodes) {
                if ($child -is [Xml.XmlElement]) {
                    Draw-SvgStroke `
                        -Graphics $renderGraphics `
                        -Element $child `
                        -Scale $scale
                }
            }
        }
        finally {
            $clipPath.Dispose()
        }

        $finalBitmap = [Drawing.Bitmap]::new(
            $Size,
            $Size,
            $pixelFormat)
        $finalGraphics = [Drawing.Graphics]::FromImage(
            $finalBitmap)
        $finalGraphics.Clear([Drawing.Color]::Transparent)
        $finalGraphics.CompositingMode =
            [Drawing.Drawing2D.CompositingMode]::SourceCopy
        $finalGraphics.CompositingQuality =
            [Drawing.Drawing2D.CompositingQuality]::HighQuality
        $finalGraphics.InterpolationMode =
            [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $finalGraphics.PixelOffsetMode =
            [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $finalGraphics.DrawImage(
            $renderBitmap,
            [Drawing.Rectangle]::new(0, 0, $Size, $Size),
            0,
            0,
            $renderSize,
            $renderSize,
            [Drawing.GraphicsUnit]::Pixel)

        $pngStream = [IO.MemoryStream]::new()
        $finalBitmap.Save(
            $pngStream,
            [Drawing.Imaging.ImageFormat]::Png)
        return [pscustomobject]@{
            Size = $Size
            Bytes = $pngStream.ToArray()
        }
    }
    finally {
        if ($null -ne $pngStream) {
            $pngStream.Dispose()
        }
        if ($null -ne $finalGraphics) {
            $finalGraphics.Dispose()
        }
        if ($null -ne $finalBitmap) {
            $finalBitmap.Dispose()
        }
        $renderGraphics.Dispose()
        $renderBitmap.Dispose()
    }
}

$sizes = @(16, 24, 32, 48, 64, 128, 256)
$frames = @($sizes | ForEach-Object {
        New-IconPngFrame -Size $_
    })
$directorySize = 6 + 16 * $frames.Count
$offset = $directorySize
$outputStream = [IO.File]::Open(
    $resolvedOutput,
    [IO.FileMode]::Create,
    [IO.FileAccess]::Write,
    [IO.FileShare]::None)
$writer = [IO.BinaryWriter]::new($outputStream)
try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$frames.Count)
    foreach ($frame in $frames) {
        $dimension = if ($frame.Size -eq 256) {
            0
        }
        else {
            $frame.Size
        }
        $writer.Write([byte]$dimension)
        $writer.Write([byte]$dimension)
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]$frame.Bytes.Length)
        $writer.Write([uint32]$offset)
        $offset += $frame.Bytes.Length
    }
    foreach ($frame in $frames) {
        $writer.Write([byte[]]$frame.Bytes)
    }
}
finally {
    $writer.Dispose()
    $outputStream.Dispose()
}

Write-Host (
    "Generated {0} from {1} with sizes {2}." -f
        $resolvedOutput,
        $resolvedSource,
        ($sizes -join ', '))
