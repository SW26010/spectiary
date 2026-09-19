param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot
)

$ErrorActionPreference = 'Stop'

$appSource = Get-Content -Raw (
    Join-Path $RepoRoot 'src/app/spectiary_app.cpp')

$reconcileTitle = [regex]::Match(
    $appSource,
    'void SpectiaryApp::ApplyLocalizedWindowTitle\(\).*?(?=void SpectiaryApp::WriteDpiConfiguration\()',
    [System.Text.RegularExpressions.RegexOptions]::Singleline)

if (-not $reconcileTitle.Success) {
    throw 'Could not locate ApplyLocalizedWindowTitle.'
}

if ($reconcileTitle.Value -match 'ui_\.AutomationView\(\)' -or
    $reconcileTitle.Value -notmatch 'ui_\.WindowTitleView\(\)') {
    throw 'Native title reconciliation must consume the narrow Shell title view instead of constructing the full automation view.'
}

$semanticMatchPosition =
    $reconcileTitle.Value.IndexOf(
        'applied_window_title_key_->Matches(title_view)',
        [StringComparison]::Ordinal)
$formatPosition =
    $reconcileTitle.Value.IndexOf(
        'FormatSpectiaryNativeWindowTitle(title_view)',
        [StringComparison]::Ordinal)
if ($semanticMatchPosition -lt 0 -or
    $formatPosition -lt 0 -or
    $semanticMatchPosition -gt $formatPosition) {
    throw 'Native title semantics must be compared before formatting or allocating the composed title.'
}

Write-Host 'Native window title architecture checks passed.'
