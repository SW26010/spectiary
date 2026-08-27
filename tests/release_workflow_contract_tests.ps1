[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

function Assert-True {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Get-JobBody {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text,
        [Parameter(Mandatory = $true)]
        [string]$JobId
    )

    $pattern = '(?ms)^  ' + [regex]::Escape($JobId) + ':\r?\n(?<body>.*?)(?=^  [A-Za-z0-9_-]+:\r?\n|\z)'
    $match = [regex]::Match($Text, $pattern)
    Assert-True -Condition $match.Success -Message "Release workflow must define job '$JobId'."
    return $match.Groups['body'].Value
}

$workflowPath = Join-Path $RepoRoot '.github\workflows\release.yml'
$presetsPath = Join-Path $RepoRoot 'CMakePresets.json'
foreach ($requiredPath in @($workflowPath, $presetsPath)) {
    Assert-True `
        -Condition (Test-Path -LiteralPath $requiredPath -PathType Leaf) `
        -Message "Required release contract source is missing: $requiredPath"
}

$workflowText = Get-Content -Raw -LiteralPath $workflowPath
$presetsText = Get-Content -Raw -LiteralPath $presetsPath
$buildBody = Get-JobBody -Text $workflowText -JobId 'build'
$publishBody = Get-JobBody -Text $workflowText -JobId 'publish'

Assert-True `
    -Condition $presetsText.Contains('"name": "vs2022-x64-release-static"') `
    -Message 'Portable release packaging must retain the Visual Studio 2022 static Release preset.'
Assert-True `
    -Condition $presetsText.Contains('"VCPKG_MANIFEST_FEATURES": ""') `
    -Message 'Every preset must clear the retired ASDF spike manifest feature from an existing CMake cache.'
Assert-True `
    -Condition ($buildBody.Contains('runs-on: windows-2022') -and -not $buildBody.Contains('runs-on: windows-latest')) `
    -Message 'The portable release build must use windows-2022 so the required Visual Studio 2022 generator is available.'
Assert-True `
    -Condition ($buildBody.Contains('scripts\build-portable-from-head.ps1') -and $buildBody.Contains('Upload release artifacts')) `
    -Message 'The release build must create the isolated HEAD package and upload its verified artifacts.'
Assert-True `
    -Condition ($publishBody.Contains('contents: write') -and $publishBody.Contains('gh release create')) `
    -Message 'The publish job must retain explicit release-write permission and the GitHub Release publication step.'

Write-Host 'Release workflow structural contract passed.'
