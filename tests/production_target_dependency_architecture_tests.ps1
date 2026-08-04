param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot
)

$ErrorActionPreference = "Stop"

function Read-RepoFile {
    param([string]$RelativePath)
    return Get-Content -Raw (Join-Path $RepoRoot $RelativePath)
}

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

$cmake = Read-RepoFile "CMakeLists.txt"
$automationState = Read-RepoFile "src/automation/automation_state.h"
$panelCacheHeader = Read-RepoFile "src/ui/panel_visibility_state_cache_io.h"
$panelStateHeader = Read-RepoFile "src/app/panel_visibility_state.h"
$workflowCoordinator = Read-RepoFile "src/ui/sample_workflow_coordinator.cpp"
$issueTextHeader = Read-RepoFile "src/ui/sample_labeling_issue_text.h"

$automationTarget = [regex]::Match(
    $cmake,
    'add_library\(specforge_automation STATIC.*?(?=\nadd_library\(specforge_sessions)',
    [System.Text.RegularExpressions.RegexOptions]::Singleline)
$sessionsTarget = [regex]::Match(
    $cmake,
    'add_library\(specforge_sessions STATIC.*?(?=\nadd_library\(specforge_desktop_ui)',
    [System.Text.RegularExpressions.RegexOptions]::Singleline)
$desktopTarget = [regex]::Match(
    $cmake,
    'add_library\(specforge_desktop_ui STATIC.*?(?=\nadd_library\(specforge_renderer)',
    [System.Text.RegularExpressions.RegexOptions]::Singleline)

Require-Match $automationTarget.Value 'src/automation/automation_state\.cpp' `
    "The automation target block must own automation_state.cpp."
Require-Match $automationTarget.Value 'target_link_libraries\(specforge_automation[\s\S]*?PUBLIC[\s\S]*?specforge_core' `
    "The automation target must retain its core dependency."
Require-NoMatch $automationTarget.Value 'specforge_sessions' `
    "The automation target must not acquire the sessions target for panel visibility."

Require-Match $sessionsTarget.Value 'src/ui/sample_workflow_coordinator\.cpp' `
    "The sessions target block must own the workflow coordinator."
Require-Match $sessionsTarget.Value 'target_link_libraries\(specforge_sessions PUBLIC specforge_core\)' `
    "The sessions target must retain its core dependency."
Require-NoMatch $sessionsTarget.Value 'specforge_desktop_ui' `
    "The sessions target must not acquire the desktop UI target for labeling text."

Require-Match $desktopTarget.Value 'src/ui/sample_workflow_panel\.cpp' `
    "The desktop UI target must own the labeling presentation adapter."
Require-Match $desktopTarget.Value 'src/ui/ui_text\.cpp' `
    "The desktop UI target must own the localized text catalog."
Require-Match $desktopTarget.Value 'target_link_libraries\(specforge_desktop_ui[\s\S]*?PUBLIC[\s\S]*?specforge_sessions' `
    "The desktop UI target must retain its sessions dependency."

Require-Match $automationState '#include "app/panel_visibility_state\.h"' `
    "Automation state must include the semantic panel visibility owner."
Require-NoMatch $automationState 'ui/panel_visibility_state_cache_io\.h' `
    "Automation state must not include the panel persistence codec."
Require-Match $panelCacheHeader '#include "app/panel_visibility_state\.h"' `
    "The panel persistence codec must consume the shared semantic state owner."

Require-NoMatch $workflowCoordinator '#include "ui/(sample_labeling_issue_text|ui_text)\.h"' `
    "The sessions coordinator must not import desktop text headers."
$issueHandler = [regex]::Match(
    $workflowCoordinator,
    'void ApplyLabelingLeaseIssue\(.*?(?=\n\}\s*// namespace)',
    [System.Text.RegularExpressions.RegexOptions]::Singleline)
Require-Match $issueHandler.Value 'outcome\.labeling_issue\s*=\s*operation\.issue' `
    "Session labeling outcomes must retain the stable semantic issue."
Require-NoMatch $issueHandler.Value 'outcome\.message|SampleLabelingIssueTextFor|UiText' `
    "Session labeling outcomes must not manufacture localized presentation text."

Require-Match $issueTextHeader 'UiTextId' `
    "The desktop issue adapter must retain stable UI text IDs."
Require-Match $issueTextHeader 'simplified_chinese' `
    "The desktop issue adapter must retain the Simplified Chinese mapping."

Write-Host "Production target dependency architecture checks passed."
