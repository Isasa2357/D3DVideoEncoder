param(
    [string]$VideoRoot = "",
    [string]$ShortSource = "",
    [string]$LongSource = "",
    [string]$FFmpegPath = "",
    [string]$FFprobePath = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

function Resolve-FullPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ""
    }
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Path))
}

function Resolve-ToolPath(
    [string]$RequestedPath,
    [string]$EnvironmentVariable,
    [string]$CommandName,
    [string]$FallbackDirectory = ""
) {
    $candidates = New-Object System.Collections.Generic.List[string]
    if (![string]::IsNullOrWhiteSpace($RequestedPath)) {
        $candidates.Add($RequestedPath)
    }
    $environmentPath = [Environment]::GetEnvironmentVariable($EnvironmentVariable)
    if (![string]::IsNullOrWhiteSpace($environmentPath)) {
        $candidates.Add($environmentPath)
    }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return Resolve-FullPath $candidate
        }
        $command = Get-Command -Name $candidate -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $command) {
            return [System.IO.Path]::GetFullPath($command.Source)
        }
        throw "$CommandName not found at requested path or command: $candidate"
    }

    if (![string]::IsNullOrWhiteSpace($FallbackDirectory)) {
        $sibling = Join-Path $FallbackDirectory $CommandName
        if (Test-Path -LiteralPath $sibling -PathType Leaf) {
            return Resolve-FullPath $sibling
        }
    }

    $fromPath = Get-Command -Name $CommandName -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $fromPath) {
        return [System.IO.Path]::GetFullPath($fromPath.Source)
    }
    throw "$CommandName not found. Pass an explicit path, set $EnvironmentVariable, or add it to PATH."
}

function Quote-Arg([string]$Value) {
    if ($Value -match '[\s"]') {
        return '"' + ($Value -replace '"', '\"') + '"'
    }
    return $Value
}

function Join-CommandLine([string]$Exe, [string[]]$Arguments) {
    $parts = New-Object System.Collections.Generic.List[string]
    $parts.Add((Quote-Arg $Exe))
    foreach ($arg in $Arguments) {
        $parts.Add((Quote-Arg $arg))
    }
    return ($parts -join " ")
}

function Get-HashString([string]$Text) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        return ([System.BitConverter]::ToString($sha.ComputeHash($bytes)) -replace "-", "").ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

function Get-FileSha256([string]$Path) {
    if (!(Test-Path -LiteralPath $Path)) {
        return ""
    }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        return ([System.BitConverter]::ToString($sha.ComputeHash($stream)) -replace "-", "").ToLowerInvariant()
    } finally {
        $stream.Dispose()
        $sha.Dispose()
    }
}

function Invoke-JsonProbe([string]$Path) {
    $json = & $script:FFprobePath -v error `
        -show_entries "format=filename,duration,size,bit_rate:stream=index,codec_type,codec_name,pix_fmt,width,height,avg_frame_rate,r_frame_rate,duration,nb_frames,bit_rate,color_range,color_space,color_transfer,color_primaries:stream_tags=rotate" `
        -of json `
        $Path
    if ($LASTEXITCODE -ne 0) {
        throw "ffprobe failed for $Path"
    }
    return ($json | ConvertFrom-Json)
}

function Get-PrimaryVideoStream($Probe) {
    foreach ($stream in $Probe.streams) {
        if ($stream.codec_type -eq "video") {
            return $stream
        }
    }
    return $null
}

function Get-AudioSummary($Probe) {
    $audio = @()
    foreach ($stream in $Probe.streams) {
        if ($stream.codec_type -eq "audio") {
            $audio += $stream.codec_name
        }
    }
    if ($audio.Count -eq 0) {
        return "none"
    }
    return ($audio -join ",")
}

function Test-Encoder([string]$Name) {
    $encoders = & $script:FFmpegPath -hide_banner -encoders 2>$null
    if ($LASTEXITCODE -ne 0) {
        return $false
    }
    return (($encoders -join "`n") -match [regex]::Escape($Name))
}

function Invoke-LoggedFfmpeg([string[]]$FfmpegArgs, [string]$LogPath) {
    $line = Join-CommandLine $script:FFmpegPath $FfmpegArgs
    Add-Content -LiteralPath $script:CommandLogPath -Value $line
    $oldErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $script:FFmpegPath @FfmpegArgs *> $LogPath
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldErrorActionPreference
    }
    if ($exitCode -ne 0) {
        $tail = ""
        if (Test-Path -LiteralPath $LogPath) {
            $tail = (Get-Content -LiteralPath $LogPath -Tail 20) -join "`n"
        }
        throw "ffmpeg failed: $line`n$tail"
    }
}

function Get-TargetForAsset([string]$RelativePath, [string]$CommandHash) {
    $target = Join-Path $script:GeneratedRoot $RelativePath
    $manifestKey = $RelativePath -replace "\\", "/"
    $existing = $script:PreviousByPath[$manifestKey]
    if (Test-Path -LiteralPath $target) {
        $properties = if ($null -ne $existing) { @($existing.PSObject.Properties.Name) } else { @() }
        $hasTrustedManifestEntry =
            $null -ne $existing -and
            $properties -contains "commandHash" -and
            $properties -contains "sha256" -and
            ![string]::IsNullOrWhiteSpace([string]$existing.sha256)
        if ($hasTrustedManifestEntry -and
            $existing.commandHash -eq $CommandHash -and
            (Get-FileSha256 $target) -eq ([string]$existing.sha256).ToLowerInvariant()) {
            return @{
                Path = $target
                RelativePath = $RelativePath
                Reused = $true
            }
        }
    }

    if (!(Test-Path -LiteralPath $target)) {
        return @{
            Path = $target
            RelativePath = $RelativePath
            Reused = $false
        }
    }

    $dir = Split-Path -Parent $RelativePath
    $name = [System.IO.Path]::GetFileNameWithoutExtension($RelativePath)
    $ext = [System.IO.Path]::GetExtension($RelativePath)
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $candidateName = $name + "_" + $stamp + $ext
    $candidateRel = if ([string]::IsNullOrWhiteSpace($dir)) { $candidateName } else { Join-Path $dir $candidateName }
    $candidate = Join-Path $script:GeneratedRoot $candidateRel
    $i = 1
    while (Test-Path -LiteralPath $candidate) {
        $candidateName = $name + "_" + $stamp + "_" + $i + $ext
        $candidateRel = if ([string]::IsNullOrWhiteSpace($dir)) { $candidateName } else { Join-Path $dir $candidateName }
        $candidate = Join-Path $script:GeneratedRoot $candidateRel
        ++$i
    }
    return @{
        Path = $candidate
        RelativePath = $candidateRel
        Reused = $false
    }
}

function New-ManifestEntry(
    [string]$RelativePath,
    [string]$Source,
    [string]$CommandLine,
    [string]$CommandHash,
    [bool]$Required,
    [string]$Result,
    [UInt64]$ExpectedSize = 0,
    [UInt32]$RawWidth = 0,
    [UInt32]$RawHeight = 0,
    [UInt32]$RawFpsNum = 0,
    [UInt32]$RawFpsDen = 0,
    [string]$RawCodec = "",
    [string]$RawPixFmt = ""
) {
    $path = Join-Path $script:GeneratedRoot $RelativePath
    $actualSize = 0
    if (Test-Path -LiteralPath $path) {
        $actualSize = (Get-Item -LiteralPath $path).Length
    }

    $duration = $null
    $width = $null
    $height = $null
    $fpsNum = $null
    $fpsDen = $null
    $codec = $RawCodec
    $pixFmt = $RawPixFmt
    $frameCount = $null

    if ($RawWidth -ne 0) {
        $width = $RawWidth
        $height = $RawHeight
        $fpsNum = $RawFpsNum
        $fpsDen = $RawFpsDen
    } elseif (Test-Path -LiteralPath $path) {
        try {
            $probe = Invoke-JsonProbe $path
            $video = Get-PrimaryVideoStream $probe
            if ($null -ne $probe.format -and ($probe.format.PSObject.Properties.Name -contains "duration")) {
                $duration = $probe.format.duration
            }
            if ($null -ne $video) {
                if ($video.PSObject.Properties.Name -contains "width") { $width = $video.width }
                if ($video.PSObject.Properties.Name -contains "height") { $height = $video.height }
                if ($video.PSObject.Properties.Name -contains "codec_name") { $codec = $video.codec_name }
                if ($video.PSObject.Properties.Name -contains "pix_fmt") { $pixFmt = $video.pix_fmt }
                if ($video.PSObject.Properties.Name -contains "nb_frames") { $frameCount = $video.nb_frames }
                if (($video.PSObject.Properties.Name -contains "avg_frame_rate") -and $video.avg_frame_rate -and $video.avg_frame_rate -match "^(\d+)/(\d+)$") {
                    $fpsNum = [UInt32]$Matches[1]
                    $fpsDen = [UInt32]$Matches[2]
                }
            }
            $probePath = Join-Path $script:LogsRoot (($RelativePath -replace '[\\/]', '_') + ".ffprobe.json")
            $probe | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $probePath -Encoding UTF8
        } catch {
            if ($Required) {
                throw
            }
        }
    }

    return [pscustomobject]@{
        relativePath = ($RelativePath -replace "\\", "/")
        source = $Source
        generationCommand = $CommandLine
        commandHash = $CommandHash
        duration = $duration
        width = $width
        height = $height
        fpsNumerator = $fpsNum
        fpsDenominator = $fpsDen
        codec = $codec
        pixelFormat = $pixFmt
        frameCount = $frameCount
        expectedSize = $ExpectedSize
        actualSize = $actualSize
        sha256 = (Get-FileSha256 $path)
        required = $Required
        result = $Result
        generationTimestamp = (Get-Date).ToUniversalTime().ToString("o")
    }
}

function Add-VideoAsset([string]$RelativePath, [string]$Source, [string[]]$FfmpegArgs, [bool]$Required) {
    $commandKey = $Source + "`n" + ($FfmpegArgs -join "`n")
    $commandHash = Get-HashString $commandKey
    $targetInfo = Get-TargetForAsset $RelativePath $commandHash
    $target = $targetInfo.Path
    $rel = $targetInfo.RelativePath
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null

    $args = @("-nostdin", "-hide_banner", "-n") + $FfmpegArgs + @($target)
    $line = Join-CommandLine $script:FFmpegPath $args
    $result = "generated"

    if ($targetInfo.Reused) {
        $result = "reused"
    } else {
        $logPath = Join-Path $script:LogsRoot (($rel -replace '[\\/]', '_') + ".ffmpeg.log")
        Invoke-LoggedFfmpeg $args $logPath
    }

    $entry = New-ManifestEntry $rel $Source $line $commandHash $Required $result
    $script:ManifestEntries.Add($entry) | Out-Null
}

function Add-RawAsset(
    [string]$RelativePath,
    [string]$Source,
    [string[]]$FfmpegArgs,
    [bool]$Required,
    [UInt32]$Width,
    [UInt32]$Height,
    [UInt32]$FpsNum,
    [UInt32]$FpsDen,
    [string]$PixFmt,
    [UInt64]$ExpectedSize
) {
    $commandKey = $Source + "`n" + ($FfmpegArgs -join "`n") + "`n" + $ExpectedSize
    $commandHash = Get-HashString $commandKey
    $targetInfo = Get-TargetForAsset $RelativePath $commandHash
    $target = $targetInfo.Path
    $rel = $targetInfo.RelativePath
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null

    $args = @("-nostdin", "-hide_banner", "-n") + $FfmpegArgs + @($target)
    $line = Join-CommandLine $script:FFmpegPath $args
    $result = "generated"
    if ($targetInfo.Reused) {
        $result = "reused"
    } else {
        $logPath = Join-Path $script:LogsRoot (($rel -replace '[\\/]', '_') + ".ffmpeg.log")
        Invoke-LoggedFfmpeg $args $logPath
    }

    $actual = (Get-Item -LiteralPath $target).Length
    if ([UInt64]$actual -ne $ExpectedSize) {
        throw "raw size mismatch for $rel expected=$ExpectedSize actual=$actual"
    }

    $entry = New-ManifestEntry $rel $Source $line $commandHash $Required $result $ExpectedSize $Width $Height $FpsNum $FpsDen "rawvideo" $PixFmt
    $script:ManifestEntries.Add($entry) | Out-Null
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-FullPath (Join-Path $scriptDir "..")
if ([string]::IsNullOrWhiteSpace($VideoRoot)) {
    if ($env:D3DVIDEOENCODER_TEST_DATA_ROOT) {
        $VideoRoot = $env:D3DVIDEOENCODER_TEST_DATA_ROOT
    } else {
        $VideoRoot = Join-Path (Split-Path -Parent $repoRoot) "video"
    }
}

$VideoRoot = Resolve-FullPath $VideoRoot
$script:FFmpegPath = Resolve-ToolPath $FFmpegPath "D3DVIDEOENCODER_FFMPEG" "ffmpeg.exe"
$script:FFprobePath = Resolve-ToolPath `
    $FFprobePath `
    "D3DVIDEOENCODER_FFPROBE" `
    "ffprobe.exe" `
    (Split-Path -Parent $script:FFmpegPath)
if (!(Test-Path -LiteralPath $VideoRoot)) {
    throw "video root not found: $VideoRoot"
}

$script:GeneratedRoot = Join-Path $VideoRoot "generated"
$script:LogsRoot = Join-Path $script:GeneratedRoot "logs"
$dirs = @(
    "normalized",
    "sizes",
    "crop",
    "formats",
    "raw",
    "reference",
    "output\baseline",
    "output\migrated",
    "logs"
)
foreach ($dir in $dirs) {
    New-Item -ItemType Directory -Force -Path (Join-Path $script:GeneratedRoot $dir) | Out-Null
}

$script:CommandLogPath = Join-Path $script:LogsRoot "commands.log"
Add-Content -LiteralPath $script:CommandLogPath -Value ("# make_test_videos " + (Get-Date).ToUniversalTime().ToString("o"))

$manifestPath = Join-Path $script:LogsRoot "generated_assets_manifest.json"
$script:PreviousByPath = @{}
if (Test-Path -LiteralPath $manifestPath) {
    try {
        $previous = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
        foreach ($entry in $previous.assets) {
            $manifestKey = ([string]$entry.relativePath) -replace "\\", "/"
            $script:PreviousByPath[$manifestKey] = $entry
        }
    } catch {
        $script:PreviousByPath = @{}
    }
}

$candidateExt = @(".mp4", ".mov", ".mkv", ".avi", ".m4v")
$candidates = @()
foreach ($file in Get-ChildItem -LiteralPath $VideoRoot -File) {
    if ($candidateExt -notcontains $file.Extension.ToLowerInvariant()) {
        continue
    }
    $probe = Invoke-JsonProbe $file.FullName
    $video = Get-PrimaryVideoStream $probe
    if ($null -eq $video -or $null -eq $probe.format.duration) {
        continue
    }
    $candidates += [pscustomobject]@{
        path = $file.FullName
        duration = [double]$probe.format.duration
        probe = $probe
    }
}

if ($candidates.Count -eq 0) {
    throw "no video source candidates found in $VideoRoot"
}

if ([string]::IsNullOrWhiteSpace($LongSource)) {
    $longMatches = @($candidates | Where-Object { [Math]::Abs($_.duration - 600.0) -le 180.0 })
    if ($longMatches.Count -ne 1) {
        $matches = ($longMatches | ForEach-Object { "$($_.path) ($($_.duration)s)" }) -join "; "
        throw "long source selection is ambiguous or missing (expected exactly one approximately 10-minute candidate, found $($longMatches.Count): $matches). Pass -LongSource explicitly."
    }
    $longCandidate = $longMatches[0]
    $LongSource = $longCandidate.path
}
if ([string]::IsNullOrWhiteSpace($ShortSource)) {
    $shortMatches = @($candidates | Where-Object { [Math]::Abs($_.duration - 10.0) -le 10.0 })
    if ($shortMatches.Count -ne 1) {
        $matches = ($shortMatches | ForEach-Object { "$($_.path) ($($_.duration)s)" }) -join "; "
        throw "short source selection is ambiguous or missing (expected exactly one approximately 10-second candidate, found $($shortMatches.Count): $matches). Pass -ShortSource explicitly."
    }
    $shortCandidate = $shortMatches[0]
    $ShortSource = $shortCandidate.path
}

$LongSource = Resolve-FullPath $LongSource
$ShortSource = Resolve-FullPath $ShortSource
if (!(Test-Path -LiteralPath $LongSource)) {
    throw "long source not found: $LongSource"
}
if (!(Test-Path -LiteralPath $ShortSource)) {
    throw "short source not found: $ShortSource"
}
if ([StringComparer]::OrdinalIgnoreCase.Equals($LongSource, $ShortSource)) {
    throw "long and short source resolved to the same file; pass distinct sources explicitly."
}

$sourceSummary = @()
foreach ($src in @($LongSource, $ShortSource) | Select-Object -Unique) {
    $probe = Invoke-JsonProbe $src
    $video = Get-PrimaryVideoStream $probe
    $sourceSummary += [pscustomobject]@{
        path = $src
        duration = $probe.format.duration
        codec = $video.codec_name
        pixelFormat = $video.pix_fmt
        width = $video.width
        height = $video.height
        avgFrameRate = $video.avg_frame_rate
        bitRate = $probe.format.bit_rate
        audio = Get-AudioSummary $probe
        colorRange = $video.color_range
        colorPrimaries = $video.color_primaries
        colorTransfer = $video.color_transfer
        colorSpace = $video.color_space
    }
    $probe | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $script:LogsRoot ((Split-Path -Leaf $src) + ".source.ffprobe.json")) -Encoding UTF8
}

$script:ManifestEntries = New-Object System.Collections.Generic.List[object]
$short = $ShortSource
$long = $LongSource

Add-VideoAsset "normalized\short_1280x720_30fps_h264_yuv420p.mp4" $short @("-v", "error", "-i", $short, "-t", "10", "-map", "0:v:0", "-an", "-vf", "scale=1280:720:flags=bicubic,fps=30,format=yuv420p", "-c:v", "libx264", "-preset", "veryfast", "-crf", "20", "-movflags", "+faststart") $true
Add-VideoAsset "normalized\long_1920x1080_30fps_h264_yuv420p.mp4" $long @("-v", "error", "-i", $long, "-map", "0:v:0", "-an", "-vf", "scale=1920:1080:flags=bicubic,fps=30,format=yuv420p", "-c:v", "libx264", "-preset", "veryfast", "-crf", "22", "-movflags", "+faststart") $true

Add-VideoAsset "sizes\short_640x360_30fps_h264.mp4" $short @("-v", "error", "-i", $short, "-t", "10", "-map", "0:v:0", "-an", "-vf", "scale=640:360:flags=bicubic,fps=30,format=yuv420p", "-c:v", "libx264", "-preset", "veryfast", "-crf", "20", "-movflags", "+faststart") $true
Add-VideoAsset "sizes\short_1920x1080_60fps_h264.mp4" $short @("-v", "error", "-i", $short, "-t", "10", "-map", "0:v:0", "-an", "-vf", "scale=1920:1080:flags=bicubic,fps=60,format=yuv420p", "-c:v", "libx264", "-preset", "veryfast", "-crf", "22", "-movflags", "+faststart") $true
Add-VideoAsset "sizes\short_3840x2160_30fps_h264.mp4" $short @("-v", "error", "-i", $short, "-t", "10", "-map", "0:v:0", "-an", "-vf", "scale=3840:2160:flags=bicubic,fps=30,format=yuv420p", "-c:v", "libx264", "-preset", "veryfast", "-crf", "26", "-movflags", "+faststart") $true
Add-VideoAsset "sizes\short_1278x718_30fps_h264.mp4" $short @("-v", "error", "-i", $short, "-t", "10", "-map", "0:v:0", "-an", "-vf", "scale=1278:718:flags=bicubic,fps=30,format=yuv420p", "-c:v", "libx264", "-preset", "veryfast", "-crf", "20", "-movflags", "+faststart") $true

Add-VideoAsset "crop\short_center_crop_720x720_h264.mp4" $short @("-v", "error", "-i", $short, "-t", "10", "-map", "0:v:0", "-an", "-vf", "crop=720:720:(iw-720)/2:(ih-720)/2,fps=30,format=yuv420p", "-c:v", "libx264", "-preset", "veryfast", "-crf", "20", "-movflags", "+faststart") $true
Add-VideoAsset "crop\short_offset_crop_1280x720_x160_y120_h264.mp4" $short @("-v", "error", "-i", $short, "-t", "10", "-map", "0:v:0", "-an", "-vf", "crop=1280:720:160:120,fps=30,format=yuv420p", "-c:v", "libx264", "-preset", "veryfast", "-crf", "20", "-movflags", "+faststart") $true
Add-VideoAsset "crop\short_portrait_720x1280_h264.mp4" $short @("-v", "error", "-i", $short, "-t", "10", "-map", "0:v:0", "-an", "-vf", "scale=720:1280:force_original_aspect_ratio=increase,crop=720:1280,fps=30,format=yuv420p", "-c:v", "libx264", "-preset", "veryfast", "-crf", "21", "-movflags", "+faststart") $true

Add-VideoAsset "formats\short_h264_high_yuv420p.mp4" $short @("-v", "error", "-i", $short, "-t", "10", "-map", "0:v:0", "-an", "-vf", "scale=1280:720:flags=bicubic,fps=30,format=yuv420p", "-c:v", "libx264", "-profile:v", "high", "-preset", "veryfast", "-crf", "20", "-movflags", "+faststart") $true

if (Test-Encoder "libx265") {
    Add-VideoAsset "formats\short_hevc_8bit_yuv420p.mp4" $short @("-v", "error", "-i", $short, "-t", "10", "-map", "0:v:0", "-an", "-vf", "scale=1280:720:flags=bicubic,fps=30,format=yuv420p", "-c:v", "libx265", "-preset", "fast", "-crf", "28", "-tag:v", "hvc1", "-movflags", "+faststart") $false
    Add-VideoAsset "formats\short_hevc_10bit_yuv420p10le.mp4" $short @("-v", "error", "-i", $short, "-t", "10", "-map", "0:v:0", "-an", "-vf", "scale=1280:720:flags=bicubic,fps=30,format=yuv420p10le", "-c:v", "libx265", "-preset", "fast", "-crf", "28", "-tag:v", "hvc1", "-movflags", "+faststart") $false
}

$frames = 300
$w = [UInt32]640
$h = [UInt32]360
Add-RawAsset "raw\short_640x360_30fps_nv12.yuv" $short @("-v", "error", "-i", $short, "-t", "10", "-map", "0:v:0", "-an", "-vf", "scale=640:360:flags=bicubic,fps=30", "-pix_fmt", "nv12", "-f", "rawvideo") $true $w $h 30 1 "nv12" ([UInt64]($w * $h * 3 / 2 * $frames))
Add-RawAsset "raw\short_640x360_30fps_p010le.yuv" $short @("-v", "error", "-i", $short, "-t", "10", "-map", "0:v:0", "-an", "-vf", "scale=640:360:flags=bicubic,fps=30", "-pix_fmt", "p010le", "-f", "rawvideo") $true $w $h 30 1 "p010le" ([UInt64]($w * $h * 3 * $frames))
Add-RawAsset "raw\short_640x360_30fps_rgba.raw" $short @("-v", "error", "-i", $short, "-t", "10", "-map", "0:v:0", "-an", "-vf", "scale=640:360:flags=bicubic,fps=30", "-pix_fmt", "rgba", "-f", "rawvideo") $true $w $h 30 1 "rgba" ([UInt64]($w * $h * 4 * $frames))
Add-RawAsset "raw\short_640x360_30fps_bgra.raw" $short @("-v", "error", "-i", $short, "-t", "10", "-map", "0:v:0", "-an", "-vf", "scale=640:360:flags=bicubic,fps=30", "-pix_fmt", "bgra", "-f", "rawvideo") $true $w $h 30 1 "bgra" ([UInt64]($w * $h * 4 * $frames))

Add-VideoAsset "reference\testsrc2_1280x720_30fps_10s_h264.mp4" "lavfi:testsrc2" @("-v", "error", "-f", "lavfi", "-i", "testsrc2=size=1280x720:rate=30:duration=10", "-map", "0:v:0", "-an", "-vf", "format=yuv420p", "-c:v", "libx264", "-preset", "veryfast", "-crf", "18", "-movflags", "+faststart") $true
Add-RawAsset "reference\testsrc2_640x360_30fps_10s_nv12.yuv" "lavfi:testsrc2" @("-v", "error", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30:duration=10", "-map", "0:v:0", "-an", "-pix_fmt", "nv12", "-f", "rawvideo") $true $w $h 30 1 "nv12" ([UInt64]($w * $h * 3 / 2 * $frames))
Add-RawAsset "reference\testsrc2_640x360_30fps_10s_p010le.yuv" "lavfi:testsrc2" @("-v", "error", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30:duration=10", "-map", "0:v:0", "-an", "-pix_fmt", "p010le", "-f", "rawvideo") $true $w $h 30 1 "p010le" ([UInt64]($w * $h * 3 * $frames))

Add-VideoAsset "reference\short_first_frame.png" $short @("-v", "error", "-i", $short, "-map", "0:v:0", "-frames:v", "1") $true
Add-VideoAsset "reference\short_frame_at_5s.png" $short @("-v", "error", "-ss", "5", "-i", $short, "-map", "0:v:0", "-frames:v", "1") $true

$manifest = [pscustomobject]@{
    repositoryRoot = $repoRoot
    videoRoot = $VideoRoot
    generatedRoot = $script:GeneratedRoot
    ffmpeg = $script:FFmpegPath
    ffprobe = $script:FFprobePath
    longSource = $LongSource
    shortSource = $ShortSource
    sourceSummary = $sourceSummary
    generatedAt = (Get-Date).ToUniversalTime().ToString("o")
    assets = $script:ManifestEntries
}

$manifest | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Host "Generated manifest: $manifestPath"
