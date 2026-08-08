param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot,

    [Parameter(Mandatory = $true)]
    [string]$BuiltExecutable
)

$ErrorActionPreference = 'Stop'

function Assert-Contains {
    param(
        [Parameter(Mandatory = $true)] [string]$Text,
        [Parameter(Mandatory = $true)] [string]$Expected,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    if (-not $Text.Contains($Expected)) {
        throw "$Description is missing '$Expected'."
    }
}

function Assert-FilesMatch {
    param(
        [Parameter(Mandatory = $true)] [string]$ExpectedPath,
        [Parameter(Mandatory = $true)] [string]$ActualPath,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    $expectedBytes = [Convert]::ToBase64String(
        [IO.File]::ReadAllBytes($ExpectedPath))
    $actualBytes = [Convert]::ToBase64String(
        [IO.File]::ReadAllBytes($ActualPath))
    if ($actualBytes -cne $expectedBytes) {
        throw "$Description does not match its generated output."
    }
}

function Read-UInt16LittleEndian {
    param(
        [Parameter(Mandatory = $true)] [byte[]]$Bytes,
        [Parameter(Mandatory = $true)] [int]$Offset
    )

    return [BitConverter]::ToUInt16($Bytes, $Offset)
}

function Read-UInt32LittleEndian {
    param(
        [Parameter(Mandatory = $true)] [byte[]]$Bytes,
        [Parameter(Mandatory = $true)] [int]$Offset
    )

    return [BitConverter]::ToUInt32($Bytes, $Offset)
}

function Read-UInt32BigEndian {
    param(
        [Parameter(Mandatory = $true)] [byte[]]$Bytes,
        [Parameter(Mandatory = $true)] [int]$Offset
    )

    return `
        ([uint32]$Bytes[$Offset] -shl 24) -bor `
        ([uint32]$Bytes[$Offset + 1] -shl 16) -bor `
        ([uint32]$Bytes[$Offset + 2] -shl 8) -bor `
        [uint32]$Bytes[$Offset + 3]
}

if ($null -eq ('SpecForgeApplicationIconNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class SpecForgeApplicationIconNative
{
    [DllImport("kernel32.dll", EntryPoint = "LoadLibraryExW", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr LoadLibraryExW(string fileName, IntPtr file, uint flags);

    [DllImport("kernel32.dll", EntryPoint = "FindResourceW", SetLastError = true)]
    public static extern IntPtr FindResourceW(IntPtr module, IntPtr name, IntPtr type);

    [DllImport("user32.dll", EntryPoint = "LoadImageW", SetLastError = true)]
    public static extern IntPtr LoadImageW(
        IntPtr instance,
        IntPtr name,
        uint type,
        int desiredWidth,
        int desiredHeight,
        uint flags);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool DestroyIcon(IntPtr icon);

    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool FreeLibrary(IntPtr module);
}
'@
}

$iconPath = Join-Path $RepoRoot 'resources\branding\specforge.ico'
$svgPath = Join-Path $RepoRoot 'resources\branding\specforge.svg'
$resourceHeaderPath = Join-Path $RepoRoot 'src\platform\specforge_resource.h'
$resourceTemplatePath = Join-Path $RepoRoot 'src\platform\specforge_resources.rc.in'
$cmakePath = Join-Path $RepoRoot 'CMakeLists.txt'
$generatorPath = Join-Path $RepoRoot 'scripts\generate-application-icon.ps1'

foreach ($requiredPath in @(
        $iconPath,
        $svgPath,
        $resourceHeaderPath,
        $resourceTemplatePath,
        $cmakePath,
        $generatorPath,
        $BuiltExecutable)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required application-icon file is missing: $requiredPath"
    }
}

$generatedIconPath = [IO.Path]::GetTempFileName()
try {
    & (Join-Path $PSHOME 'powershell.exe') `
        -NoProfile `
        -ExecutionPolicy Bypass `
        -File $generatorPath `
        -Source $svgPath `
        -Output $generatedIconPath
    if ($LASTEXITCODE -ne 0) {
        throw "Application icon generator failed with exit code $LASTEXITCODE."
    }
    Assert-FilesMatch `
        -ExpectedPath $iconPath `
        -ActualPath $generatedIconPath `
        -Description 'Committed application ICO'
}
finally {
    Remove-Item -LiteralPath $generatedIconPath -Force `
        -ErrorAction SilentlyContinue
}

$iconBytes = [IO.File]::ReadAllBytes($iconPath)
if ($iconBytes.Length -lt 6 -or
    (Read-UInt16LittleEndian $iconBytes 0) -ne 0 -or
    (Read-UInt16LittleEndian $iconBytes 2) -ne 1) {
    throw 'Application ICO header is invalid.'
}

$entryCount = Read-UInt16LittleEndian $iconBytes 4
$requiredSizes = @(16, 24, 32, 48, 64, 128, 256)
if ($entryCount -ne $requiredSizes.Count) {
    throw "Application ICO expected $($requiredSizes.Count) entries; found $entryCount."
}

$actualSizes = @()
$pngSignature = [byte[]](0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a)
for ($index = 0; $index -lt $entryCount; ++$index) {
    $entryOffset = 6 + 16 * $index
    $width = if ($iconBytes[$entryOffset] -eq 0) {
        256
    }
    else {
        [int]$iconBytes[$entryOffset]
    }
    $height = if ($iconBytes[$entryOffset + 1] -eq 0) {
        256
    }
    else {
        [int]$iconBytes[$entryOffset + 1]
    }
    $planes = Read-UInt16LittleEndian $iconBytes ($entryOffset + 4)
    $bitsPerPixel = Read-UInt16LittleEndian $iconBytes ($entryOffset + 6)
    $payloadSize = Read-UInt32LittleEndian $iconBytes ($entryOffset + 8)
    $payloadOffset = Read-UInt32LittleEndian $iconBytes ($entryOffset + 12)

    if ($width -ne $height -or $planes -ne 1 -or $bitsPerPixel -ne 32) {
        throw "Application ICO entry $index has invalid dimensions or color metadata."
    }
    if ($payloadOffset + $payloadSize -gt $iconBytes.Length -or
        $payloadSize -lt 24) {
        throw "Application ICO entry $index has an invalid payload range."
    }
    for ($signatureIndex = 0; $signatureIndex -lt $pngSignature.Length; ++$signatureIndex) {
        if ($iconBytes[$payloadOffset + $signatureIndex] -ne
            $pngSignature[$signatureIndex]) {
            throw "Application ICO entry $index is not PNG-compressed."
        }
    }
    $pngWidth = Read-UInt32BigEndian $iconBytes ($payloadOffset + 16)
    $pngHeight = Read-UInt32BigEndian $iconBytes ($payloadOffset + 20)
    if ($pngWidth -ne $width -or $pngHeight -ne $height) {
        throw "Application ICO entry $index PNG dimensions do not match its directory entry."
    }
    $actualSizes += $width
}

if ((Compare-Object $requiredSizes ($actualSizes | Sort-Object -Unique))) {
    throw "Application ICO sizes are incomplete: $($actualSizes -join ', ')."
}

$resourceHeader = Get-Content -Raw -LiteralPath $resourceHeaderPath
$resourceIdMatch = [regex]::Match(
    $resourceHeader,
    '#define\s+SPECFORGE_RESOURCE_APPLICATION_ICON\s+(\d+)')
if (-not $resourceIdMatch.Success) {
    throw 'Application icon resource identifier is missing.'
}
$resourceId = [int]$resourceIdMatch.Groups[1].Value

$loadLibraryAsDataFile = 0x00000002
$loadLibraryAsImageResource = 0x00000020
$module = [SpecForgeApplicationIconNative]::LoadLibraryExW(
    (Resolve-Path -LiteralPath $BuiltExecutable).Path,
    [IntPtr]::Zero,
    $loadLibraryAsDataFile -bor $loadLibraryAsImageResource)
if ($module -eq [IntPtr]::Zero) {
    throw "Could not open SpecForge executable resources: $BuiltExecutable"
}

try {
    $groupIcon = [SpecForgeApplicationIconNative]::FindResourceW(
        $module,
        [IntPtr]$resourceId,
        [IntPtr]14)
    if ($groupIcon -eq [IntPtr]::Zero) {
        throw "SpecForge executable is missing group icon resource $resourceId."
    }

    foreach ($size in @(16, 32)) {
        $loadedIcon = [SpecForgeApplicationIconNative]::LoadImageW(
            $module,
            [IntPtr]$resourceId,
            1,
            $size,
            $size,
            0)
        if ($loadedIcon -eq [IntPtr]::Zero) {
            throw "SpecForge executable icon resource $resourceId could not load at ${size}px."
        }
        [void][SpecForgeApplicationIconNative]::DestroyIcon($loadedIcon)
    }
}
finally {
    [void][SpecForgeApplicationIconNative]::FreeLibrary($module)
}

$resourceTemplate = Get-Content -Raw -LiteralPath $resourceTemplatePath
$cmakeSource = Get-Content -Raw -LiteralPath $cmakePath

Assert-Contains $resourceTemplate `
    'SPECFORGE_RESOURCE_APPLICATION_ICON ICON "@SPECFORGE_APPLICATION_ICON@"' `
    'Application icon resource template'
Assert-Contains $cmakeSource `
    '${SPECFORGE_APPLICATION_ICON}' `
    'Application icon resource dependency'

Write-Host 'Application icon asset, resource, and integration checks passed.'
