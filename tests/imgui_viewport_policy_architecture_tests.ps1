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
    'io\.ConfigViewportsNoTaskBarIcon\s*=\s*true') {
    throw 'Detached panels must remain auxiliary windows without independent taskbar or Alt+Tab entries.'
}

$taskbarAssignments = [regex]::Matches(
    $appSource,
    'ConfigViewportsNoTaskBarIcon\s*=')

if ($taskbarAssignments.Count -ne 1) {
    throw 'ConfigViewportsNoTaskBarIcon should have exactly one policy assignment.'
}

if ($appSource -notmatch
    'io\.ConfigViewportsNoDefaultParent\s*=\s*false') {
    throw 'Detached panels must keep independent ImGui viewports while their Win32 platform windows remain owned by the main window.'
}

$parentAssignments = [regex]::Matches(
    $appSource,
    'ConfigViewportsNoDefaultParent\s*=')

if ($parentAssignments.Count -ne 1) {
    throw 'ConfigViewportsNoDefaultParent should have exactly one policy assignment.'
}

Write-Host 'ImGui viewport policy architecture checks passed.'
