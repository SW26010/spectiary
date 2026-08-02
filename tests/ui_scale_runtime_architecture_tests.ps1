param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot
)

$ErrorActionPreference = "Stop"

$appSource = Get-Content -Raw (
    Join-Path $RepoRoot "src/app/specforge_app.cpp")

$startRecording = [regex]::Match(
    $appSource,
    'bool SpecForgeApp::StartProfileRecording\(.*?(?=void SpecForgeApp::StopProfileRecording\()',
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

$pendingSettingsApplication = [regex]::Match(
    $appSource,
    'void SpecForgeApp::ApplyPendingApplicationSettings\(.*?(?=void SpecForgeApp::ServiceAutomationSettingGet\()',
    [System.Text.RegularExpressions.RegexOptions]::Singleline)

if (-not $pendingSettingsApplication.Success -or
    $pendingSettingsApplication.Value -notmatch
        'TakeAppliedUiScalePercentage\(\)' -or
    $pendingSettingsApplication.Value -notmatch
        'ApplyUiScale\(' -or
    $pendingSettingsApplication.Value -notmatch
        'TakeAppliedUiLanguage\(\)' -or
    $pendingSettingsApplication.Value -notmatch
        'ApplyLocalizedWindowTitle\(\)' -or
    $pendingSettingsApplication.Value -notmatch
        'render_wake_scheduler_\.RequestFrame\(\)') {
    throw "Pending production setting notifications must have one App-owned automation drain."
}

$serviceAutomation = [regex]::Match(
    $appSource,
    'void SpecForgeApp::ServiceAutomation\(\).*?(?=std::optional<RenderWakeScheduler::TimePoint>)',
    [System.Text.RegularExpressions.RegexOptions]::Singleline)

if (-not $serviceAutomation.Success -or
    $serviceAutomation.Value -notmatch
        'ApplyPendingApplicationSettings\(\s*"user_scale_changed"\s*\)[\s\S]*?PollAutomationBusinessOperations\(\)') {
    throw "Automation dispatch must apply pending production setting notifications before terminal or state observation."
}

$settingSet = [regex]::Match(
    $appSource,
    'void SpecForgeApp::ServiceAutomationSettingSet\(.*?(?=void SpecForgeApp::BeginAutomationSourceOpen\()',
    [System.Text.RegularExpressions.RegexOptions]::Singleline)

if (-not $settingSet.Success -or
    $settingSet.Value -match
        'if\s*\(\s*result\.applied\(\)\s*\)' -or
    $settingSet.Value -notmatch
        'ApplyPendingApplicationSettings\(\s*"automation_setting_changed"\s*\)[\s\S]*?automation_server_->Complete\(') {
    throw "Successful setting.set paths, including Unchanged, must apply pending production notifications before completion."
}

Write-Host "UI scale runtime architecture checks passed."
