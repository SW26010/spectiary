[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot,

    [Parameter(Mandatory = $true)]
    [string]$DebugBuildDirectory,

    [Parameter(Mandatory = $true)]
    [string]$ReleaseBuildDirectory
)

$ErrorActionPreference = 'Stop'

& (Join-Path $RepoRoot 'tests\helpers\release_contract_validation.ps1') `
    -RepoRoot $RepoRoot `
    -DebugBuildDirectory $DebugBuildDirectory `
    -ReleaseBuildDirectory $ReleaseBuildDirectory `
    -ContractArea ConfiguredBuild
