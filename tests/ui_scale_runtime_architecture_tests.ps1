param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot
)

$ErrorActionPreference = "Stop"

$appSource = Get-Content -Raw (
    Join-Path $RepoRoot "src/app/specforge_app.cpp")

$startRecording = [regex]::Match(
    $appSource,
    'void SpecForgeApp::StartProfileRecording\(.*?(?=void SpecForgeApp::StopProfileRecording\()',
    [System.Text.RegularExpressions.RegexOptions]::Singleline)

if (-not $startRecording.Success) {
    throw "Could not locate StartProfileRecording."
}

if ($startRecording.Value -notmatch
    'WriteDpiConfiguration\(\s*"recording_started"\s*\)') {
    throw "Starting a recording must snapshot the current DPI and UI scale configuration."
}

$dpiConfiguration = [regex]::Match(
    $appSource,
    'void SpecForgeApp::WriteDpiConfiguration\(.*?(?=void SpecForgeApp::ToggleFullscreen\()',
    [System.Text.RegularExpressions.RegexOptions]::Singleline)

if (-not $dpiConfiguration.Success -or
    $dpiConfiguration.Value -notmatch
        'profile_\.WriteEvent\(\s*"dpi_config"' -or
    $dpiConfiguration.Value -notmatch
        '"system_dpi_scale"' -or
    $dpiConfiguration.Value -notmatch
        '"user_scale"' -or
    $dpiConfiguration.Value -notmatch
        '"effective_scale"') {
    throw "The recording-start DPI snapshot must include system, user, and effective scale."
}

Write-Host "UI scale runtime architecture checks passed."
