param(
    [string] $ProjectRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$outputDir = Join-Path $PSScriptRoot 'PlayStore'
$backgroundFile = Join-Path $outputDir 'feature-background-generated.png'
$iconFile = Join-Path $ProjectRoot 'Assets\icon-android.png'
$instrumentScreenshot = Join-Path $PSScriptRoot 'ChurchDAW-1.3.0-TANK3-Final.png'
$librariesScreenshot = Join-Path $PSScriptRoot 'ChurchDAW-1.3.0-TANK3-Final-Libraries.png'
$reverbScreenshot = Join-Path $PSScriptRoot 'ChurchDAW-1.3.0-TANK3-Reverb-Shimmer.png'

foreach ($requiredFile in @(
    $backgroundFile,
    $iconFile,
    $instrumentScreenshot,
    $librariesScreenshot,
    $reverbScreenshot
)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Arquivo necessario ausente: $requiredFile"
    }
}

New-Item -ItemType Directory -Path $outputDir -Force | Out-Null

function Set-HighQualityGraphics {
    param([System.Drawing.Graphics] $Graphics)

    $Graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $Graphics.InterpolationMode =
        [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $Graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $Graphics.CompositingQuality =
        [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $Graphics.TextRenderingHint =
        [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
}

function New-RoundedPath {
    param(
        [System.Drawing.RectangleF] $Rectangle,
        [float] $Radius
    )

    $diameter = $Radius * 2.0
    $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
    $path.AddArc($Rectangle.X, $Rectangle.Y, $diameter, $diameter, 180, 90)
    $path.AddArc(
        $Rectangle.Right - $diameter,
        $Rectangle.Y,
        $diameter,
        $diameter,
        270,
        90
    )
    $path.AddArc(
        $Rectangle.Right - $diameter,
        $Rectangle.Bottom - $diameter,
        $diameter,
        $diameter,
        0,
        90
    )
    $path.AddArc(
        $Rectangle.X,
        $Rectangle.Bottom - $diameter,
        $diameter,
        $diameter,
        90,
        90
    )
    $path.CloseFigure()
    return $path
}

function Draw-CoverImage {
    param(
        [System.Drawing.Graphics] $Graphics,
        [System.Drawing.Image] $Image,
        [System.Drawing.RectangleF] $Destination
    )

    $scale = [Math]::Max(
        $Destination.Width / $Image.Width,
        $Destination.Height / $Image.Height
    )
    $sourceWidth = $Destination.Width / $scale
    $sourceHeight = $Destination.Height / $scale
    $sourceX = ($Image.Width - $sourceWidth) / 2.0
    $sourceY = ($Image.Height - $sourceHeight) / 2.0
    $source = [System.Drawing.RectangleF]::new(
        [float] $sourceX,
        [float] $sourceY,
        [float] $sourceWidth,
        [float] $sourceHeight
    )
    $Graphics.DrawImage(
        $Image,
        $Destination,
        $source,
        [System.Drawing.GraphicsUnit]::Pixel
    )
}

function Draw-ContainedRoundedImage {
    param(
        [System.Drawing.Graphics] $Graphics,
        [System.Drawing.Image] $Image,
        [System.Drawing.RectangleF] $Destination,
        [float] $Radius
    )

    $shadowRect = [System.Drawing.RectangleF]::new(
        $Destination.X + 8,
        $Destination.Y + 12,
        $Destination.Width,
        $Destination.Height
    )
    $shadowPath = New-RoundedPath -Rectangle $shadowRect -Radius $Radius
    $shadowBrush = [System.Drawing.SolidBrush]::new(
        [System.Drawing.Color]::FromArgb(150, 0, 0, 0)
    )
    $Graphics.FillPath($shadowBrush, $shadowPath)
    $shadowBrush.Dispose()
    $shadowPath.Dispose()

    $clipPath = New-RoundedPath -Rectangle $Destination -Radius $Radius
    $saved = $Graphics.Save()
    $Graphics.SetClip($clipPath)
    $Graphics.FillRectangle(
        [System.Drawing.Brushes]::Black,
        $Destination.X,
        $Destination.Y,
        $Destination.Width,
        $Destination.Height
    )

    $imageScale = [Math]::Min(
        $Destination.Width / $Image.Width,
        $Destination.Height / $Image.Height
    )
    $drawWidth = $Image.Width * $imageScale
    $drawHeight = $Image.Height * $imageScale
    $drawRect = [System.Drawing.RectangleF]::new(
        [float] ($Destination.X + ($Destination.Width - $drawWidth) / 2.0),
        [float] ($Destination.Y + ($Destination.Height - $drawHeight) / 2.0),
        [float] $drawWidth,
        [float] $drawHeight
    )
    $Graphics.DrawImage($Image, $drawRect)
    $Graphics.Restore($saved)

    $borderPen = [System.Drawing.Pen]::new(
        [System.Drawing.Color]::FromArgb(230, 255, 38, 86),
        3.0
    )
    $Graphics.DrawPath($borderPen, $clipPath)
    $borderPen.Dispose()
    $clipPath.Dispose()
}

function Save-Png {
    param(
        [System.Drawing.Bitmap] $Bitmap,
        [string] $Path
    )

    $temporaryPath = "$Path.tmp-$([Guid]::NewGuid().ToString('N'))"

    try {
        $Bitmap.Save($temporaryPath, [System.Drawing.Imaging.ImageFormat]::Png)
        $sameBytes = (Test-Path -LiteralPath $Path -PathType Leaf) -and
            (Get-Item -LiteralPath $Path).Length -eq
                (Get-Item -LiteralPath $temporaryPath).Length -and
            (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -eq
                (Get-FileHash -LiteralPath $temporaryPath -Algorithm SHA256).Hash

        # Nao mude o mtime de um recurso identico: app-icon-512.png tambem e
        # entrada do AAB, e uma regeneracao visual sem mudanca real nao deve
        # produzir um falso "bundle obsoleto" no verificador Android.
        if (-not $sameBytes) {
            Copy-Item -LiteralPath $temporaryPath -Destination $Path -Force
        }
    }
    finally {
        if (Test-Path -LiteralPath $temporaryPath) {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
}

function New-StoreScreenshot {
    param(
        [string] $SourcePath,
        [string] $OutputName,
        [string] $Title,
        [string] $Subtitle
    )

    $background = [System.Drawing.Image]::FromFile($backgroundFile)
    $source = [System.Drawing.Image]::FromFile($SourcePath)
    $icon = [System.Drawing.Image]::FromFile($iconFile)
    $canvas = [System.Drawing.Bitmap]::new(
        1920,
        1080,
        # A Play rejeita screenshots com canal alpha, mesmo totalmente opaco.
        [System.Drawing.Imaging.PixelFormat]::Format24bppRgb
    )
    $graphics = [System.Drawing.Graphics]::FromImage($canvas)

    try {
        Set-HighQualityGraphics -Graphics $graphics
        Draw-CoverImage -Graphics $graphics -Image $background `
            -Destination ([System.Drawing.RectangleF]::new(0, 0, 1920, 1080))

        $veil = [System.Drawing.SolidBrush]::new(
            [System.Drawing.Color]::FromArgb(176, 4, 5, 8)
        )
        $graphics.FillRectangle($veil, 0, 0, 1920, 1080)
        $veil.Dispose()

        $accent = [System.Drawing.SolidBrush]::new(
            [System.Drawing.Color]::FromArgb(255, 255, 38, 86)
        )
        $white = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::White)
        $muted = [System.Drawing.SolidBrush]::new(
            [System.Drawing.Color]::FromArgb(255, 190, 193, 201)
        )
        $titleFont = [System.Drawing.Font]::new(
            'Segoe UI',
            52,
            [System.Drawing.FontStyle]::Bold,
            [System.Drawing.GraphicsUnit]::Pixel
        )
        $subtitleFont = [System.Drawing.Font]::new(
            'Segoe UI',
            27,
            [System.Drawing.FontStyle]::Regular,
            [System.Drawing.GraphicsUnit]::Pixel
        )
        $brandFont = [System.Drawing.Font]::new(
            'Segoe UI',
            28,
            [System.Drawing.FontStyle]::Bold -bor [System.Drawing.FontStyle]::Italic,
            [System.Drawing.GraphicsUnit]::Pixel
        )

        $graphics.FillRectangle($accent, 80, 48, 76, 6)
        $graphics.DrawString($Title, $titleFont, $white, 80, 64)
        $graphics.DrawString($Subtitle, $subtitleFont, $muted, 82, 130)
        $graphics.DrawImage($icon, 1740, 46, 90, 90)
        $graphics.DrawString('ChurchDAW', $brandFont, $white, 1548, 76)

        $screenRect = [System.Drawing.RectangleF]::new(80, 226, 1760, 774)
        Draw-ContainedRoundedImage -Graphics $graphics -Image $source `
            -Destination $screenRect -Radius 28

        $brandFont.Dispose()
        $subtitleFont.Dispose()
        $titleFont.Dispose()
        $muted.Dispose()
        $white.Dispose()
        $accent.Dispose()

        Save-Png -Bitmap $canvas -Path (Join-Path $outputDir $OutputName)
    }
    finally {
        $graphics.Dispose()
        $canvas.Dispose()
        $icon.Dispose()
        $source.Dispose()
        $background.Dispose()
    }
}

# Ícone 512 x 512 exigido pela ficha da Play Store. A arte de origem permanece
# intacta; este arquivo é somente a derivação de publicação.
$iconSource = [System.Drawing.Image]::FromFile($iconFile)
$storeIcon = [System.Drawing.Bitmap]::new(
    512,
    512,
    [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
)
$iconGraphics = [System.Drawing.Graphics]::FromImage($storeIcon)
try {
    Set-HighQualityGraphics -Graphics $iconGraphics
    $iconGraphics.Clear([System.Drawing.Color]::Black)
    $iconGraphics.DrawImage($iconSource, 0, 0, 512, 512)
    Save-Png -Bitmap $storeIcon -Path (Join-Path $outputDir 'app-icon-512.png')
}
finally {
    $iconGraphics.Dispose()
    $storeIcon.Dispose()
    $iconSource.Dispose()
}

# Arte de destaque 1024 x 500. A tela inserida é uma captura real do app; a
# imagem gerada é usada somente como fundo de marca.
$background = [System.Drawing.Image]::FromFile($backgroundFile)
$instrument = [System.Drawing.Image]::FromFile($instrumentScreenshot)
$feature = [System.Drawing.Bitmap]::new(
    1024,
    500,
    [System.Drawing.Imaging.PixelFormat]::Format24bppRgb
)
$featureGraphics = [System.Drawing.Graphics]::FromImage($feature)
try {
    Set-HighQualityGraphics -Graphics $featureGraphics
    Draw-CoverImage -Graphics $featureGraphics -Image $background `
        -Destination ([System.Drawing.RectangleF]::new(0, 0, 1024, 500))

    $leftGradient = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
        [System.Drawing.PointF]::new(0, 0),
        [System.Drawing.PointF]::new(470, 0),
        [System.Drawing.Color]::FromArgb(245, 5, 6, 9),
        [System.Drawing.Color]::FromArgb(25, 5, 6, 9)
    )
    $featureGraphics.FillRectangle($leftGradient, 0, 0, 530, 500)
    $leftGradient.Dispose()

    $white = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::White)
    $red = [System.Drawing.SolidBrush]::new(
        [System.Drawing.Color]::FromArgb(255, 255, 38, 86)
    )
    $muted = [System.Drawing.SolidBrush]::new(
        [System.Drawing.Color]::FromArgb(255, 204, 207, 214)
    )
    $brandFont = [System.Drawing.Font]::new(
        'Segoe UI',
        42,
        [System.Drawing.FontStyle]::Bold -bor [System.Drawing.FontStyle]::Italic,
        [System.Drawing.GraphicsUnit]::Pixel
    )
    $eyebrowFont = [System.Drawing.Font]::new(
        'Segoe UI',
        14,
        [System.Drawing.FontStyle]::Bold,
        [System.Drawing.GraphicsUnit]::Pixel
    )
    $taglineFont = [System.Drawing.Font]::new(
        'Segoe UI',
        25,
        [System.Drawing.FontStyle]::Bold,
        [System.Drawing.GraphicsUnit]::Pixel
    )
    $platformFont = [System.Drawing.Font]::new(
        'Segoe UI',
        14,
        [System.Drawing.FontStyle]::Bold,
        [System.Drawing.GraphicsUnit]::Pixel
    )

    $typographic = [System.Drawing.StringFormat]::GenericTypographic
    $featureGraphics.DrawString('Church', $brandFont, $white, 46, 88, $typographic)
    $churchWidth = $featureGraphics.MeasureString(
        'Church',
        $brandFont,
        [System.Drawing.PointF]::new(46, 88),
        $typographic
    ).Width
    $featureGraphics.DrawString(
        'DAW',
        $brandFont,
        $red,
        46 + $churchWidth,
        88,
        $typographic
    )
    $bullet = [char] 0x2022
    $featureGraphics.DrawString(
        "TOQUE  $bullet  CRIE  $bullet  ADORE",
        $eyebrowFont,
        $red,
        49,
        150
    )
    $featureGraphics.DrawString(
        "A plataforma de timbres`npara worship.",
        $taglineFont,
        $white,
        46,
        186
    )
    $audioLabel = ([char] 0x00c1) + 'UDIO'
    $featureGraphics.DrawString(
        "ANDROID  $bullet  MIDI  $bullet  $audioLabel",
        $platformFont,
        $muted,
        48,
        278
    )

    $featureScreen = [System.Drawing.RectangleF]::new(414, 118, 574, 252)
    Draw-ContainedRoundedImage -Graphics $featureGraphics -Image $instrument `
        -Destination $featureScreen -Radius 18

    $platformFont.Dispose()
    $taglineFont.Dispose()
    $eyebrowFont.Dispose()
    $brandFont.Dispose()
    $muted.Dispose()
    $red.Dispose()
    $white.Dispose()

    Save-Png -Bitmap $feature -Path (Join-Path $outputDir 'feature-graphic-1024x500.png')
}
finally {
    $featureGraphics.Dispose()
    $feature.Dispose()
    $instrument.Dispose()
    $background.Dispose()
}

New-StoreScreenshot `
    -SourcePath $instrumentScreenshot `
    -OutputName 'screenshot-01-performance-1920x1080.png' `
    -Title 'Toque com liberdade' `
    -Subtitle 'Pianos, pads e camadas em tempo real'

New-StoreScreenshot `
    -SourcePath $librariesScreenshot `
    -OutputName 'screenshot-02-libraries-1920x1080.png' `
    -Title 'Bibliotecas sob demanda' `
    -Subtitle 'App leve. Baixe os timbres quando quiser.'

New-StoreScreenshot `
    -SourcePath $reverbScreenshot `
    -OutputName 'screenshot-03-master-1920x1080.png' `
    -Title 'Ambientes worship de verdade' `
    -Subtitle 'Cloud, Shimmer, Hall, Plate e muito mais'

$outputs = @(
    'app-icon-512.png',
    'feature-graphic-1024x500.png',
    'screenshot-01-performance-1920x1080.png',
    'screenshot-02-libraries-1920x1080.png',
    'screenshot-03-master-1920x1080.png'
)

foreach ($name in $outputs) {
    $path = Join-Path $outputDir $name
    $image = [System.Drawing.Image]::FromFile($path)
    try {
        [pscustomobject]@{
            File = $path
            Width = $image.Width
            Height = $image.Height
            PixelFormat = [string] $image.PixelFormat
            Bytes = (Get-Item -LiteralPath $path).Length
            Sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        }
    }
    finally {
        $image.Dispose()
    }
}
