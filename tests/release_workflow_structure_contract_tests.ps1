[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot
)

$ErrorActionPreference = 'Stop'

& (Join-Path $RepoRoot 'tests\helpers\release_contract_validation.ps1') `
    -RepoRoot $RepoRoot `
    -ContractArea WorkflowStructure
