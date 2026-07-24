param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot
)

$ErrorActionPreference = "Stop"

$panelSource = Get-Content -Raw (
    Join-Path $RepoRoot "src/ui/source_collection_panel.cpp")
$panelHeader = Get-Content -Raw (
    Join-Path $RepoRoot "src/ui/source_collection_panel.h")

function Require-Match {
    param(
        [string]$Text,
        [string]$Pattern,
        [string]$Message
    )

    if ($Text -notmatch $Pattern) {
        throw $Message
    }
}

function Require-NoMatch {
    param(
        [string]$Text,
        [string]$Pattern,
        [string]$Message
    )

    if ($Text -match $Pattern) {
        throw $Message
    }
}

Require-NoMatch `
    $panelSource `
    'SourceCollectionSessionView\s+view\s*=\s*interaction\.View\(\)' `
    "Panel render paths must not copy the full SourceCollectionSessionView."

Require-Match `
    $panelSource `
    'const\s+SourceCollectionSessionView&\s+view\s*=\s*interaction\.View\(\)' `
    "RenderFiles should borrow the immutable session view."

Require-Match `
    $panelSource `
    'SourceCollectionNavigationView\s+navigation\s*=\s*interaction\.View\(\)\.navigation' `
    "RenderNavigation should copy only its mutable navigation projection."

Require-Match `
    $panelHeader `
    'RenderSampleNameSearch\(\s*SourceCollectionNavigationView\s+navigation' `
    "The sample-name helper should accept only the navigation projection."

Require-Match `
    $panelHeader `
    'BeginSampleNameSearch\(\s*const\s+SourceCollectionNavigationView&\s+navigation' `
    "The sample-name search initializer should borrow only navigation state."

Write-Host "Panel session render architecture checks passed."
