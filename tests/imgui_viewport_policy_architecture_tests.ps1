param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot
)

$ErrorActionPreference = 'Stop'

$appSource = Get-Content -Raw (
    Join-Path $RepoRoot 'src/app/specforge_app.cpp')

if ($appSource -notmatch
    'io\.ConfigFlags\s*\|=\s*ImGuiConfigFlags_ViewportsEnable') {
    throw 'Multi-viewport must remain enabled.'
}

if ($appSource -notmatch
    'io\.ConfigViewportsNoAutoMerge\s*=\s*true') {
    throw 'Floating windows must remain independent viewports; see #35.'
}

$assignments = [regex]::Matches(
    $appSource,
    'ConfigViewportsNoAutoMerge\s*=')

if ($assignments.Count -ne 1) {
    throw 'ConfigViewportsNoAutoMerge should have exactly one policy assignment.'
}

if ($appSource -notmatch
    'io\.ConfigViewportsNoDefaultParent\s*=\s*true') {
    throw 'Detached platform windows must remain independent top-level HWNDs; main-window lifecycle is synchronized explicitly.'
}

$parentAssignments = [regex]::Matches(
    $appSource,
    'ConfigViewportsNoDefaultParent\s*=')

if ($parentAssignments.Count -ne 1) {
    throw 'ConfigViewportsNoDefaultParent should have exactly one policy assignment.'
}

Write-Host 'ImGui viewport policy architecture checks passed.'
