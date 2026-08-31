param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot
)

$ErrorActionPreference = "Stop"

$panelSource = Get-Content -Raw (
    Join-Path $RepoRoot "src/ui/source_collection_panel.cpp")
$panelHeader = Get-Content -Raw (
    Join-Path $RepoRoot "src/ui/source_collection_panel.h")
$shellSource = Get-Content -Raw (
    Join-Path $RepoRoot "src/ui/shell_ui.cpp")

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

function Require-LiteralOrder {
    param(
        [string]$Text,
        [string[]]$Tokens,
        [string]$Message
    )

    $offset = 0
    foreach ($token in $Tokens) {
        $index = $Text.IndexOf($token, $offset)
        if ($index -lt 0) {
            throw "$Message Missing or out-of-order token: $token"
        }
        $offset = $index + $token.Length
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

$renderStart = $shellSource.IndexOf(
    "void ShellUi::Render(const ShellStatus& status)")
if ($renderStart -lt 0) {
    throw "ShellUi::Render source start was not found."
}
$renderEnd = $shellSource.IndexOf(
    "void ShellUi::RunMaintenance",
    $renderStart)
if ($renderEnd -lt 0) {
    throw "ShellUi::Render source end was not found."
}
$renderBody = $shellSource.Substring(
    $renderStart,
    $renderEnd - $renderStart)
$ordinaryStart = $renderBody.IndexOf("RenderDockHost(status);")
if ($ordinaryStart -lt 0) {
    throw "ShellUi::Render ordinary-mode boundary was not found."
}
$immersiveBody = $renderBody.Substring(0, $ordinaryStart)
$ordinaryBody = $renderBody.Substring($ordinaryStart)

Require-LiteralOrder `
    $immersiveBody `
    @(
        "RenderImmersivePlot(status);",
        "FinalizeTaskNameEdit(",
        "FinalizeNavigationInputEdits(",
        "TakeAction()",
        "HandleSampleWorkflowShortcut();",
        "return;"
    ) `
    "Immersive rendering must finalize hidden Navigation edits before dispatch and return."

Require-LiteralOrder `
    $ordinaryBody `
    @(
        "RenderNavigationPanel(",
        "RenderFiltersPanel(",
        "RenderSortingPanel(",
        "if (!application_settings_.View().panel_visibility.labeling)",
        "FinalizeTaskNameEdit(",
        "FinalizeNavigationInputEdits(",
        "TakeAction()",
        "HandleSampleWorkflowShortcut();"
    ) `
    "Ordinary rendering must finalize Navigation edits after Filters and Sorting, then dispatch them."

$finalizeCount = [regex]::Matches(
    $renderBody,
    'FinalizeNavigationInputEdits\s*\(').Count
if ($finalizeCount -ne 2) {
    throw "ShellUi::Render must contain exactly two Navigation finalization sites; found $finalizeCount."
}

$taskNameFinalizeCount = [regex]::Matches(
    $renderBody,
    'FinalizeTaskNameEdit\s*\(').Count
if ($taskNameFinalizeCount -ne 2) {
    throw "ShellUi::Render must finalize task-name edits in both hidden-panel paths; found $taskNameFinalizeCount sites."
}

Write-Host "Panel session render architecture checks passed."
