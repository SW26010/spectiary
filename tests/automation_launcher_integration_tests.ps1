[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Launcher,
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [Parameter(Mandatory = $true)]
    [string]$CleanupFixture
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

function Assert-True {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Get-FileSha256 {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $algorithm =
        [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::Open(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite)
    try {
        return [System.BitConverter]::ToString(
            $algorithm.ComputeHash($stream)).Replace('-', '')
    }
    finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
}

function Get-TreeFingerprint {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return 'missing'
    }
    $root = (Resolve-Path -LiteralPath $Path).Path
    $entries = @(
        Get-ChildItem -LiteralPath $root -Force -Recurse |
            Sort-Object FullName |
            ForEach-Object {
                $relative = $_.FullName.Substring($root.Length)
                if ($_.PSIsContainer) {
                    "D|$relative|$($_.LastWriteTimeUtc.Ticks)"
                }
                else {
                    $hash =
                        Get-FileSha256 -Path $_.FullName
                    "F|$relative|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)|$hash"
                }
            }
    )
    return ($entries -join "`n")
}

function Get-OrdinaryStateRoot {
    param(
        [Parameter(Mandatory = $true)]
        [string]$AppPath
    )

    $appDirectory = Split-Path -Parent $AppPath
    $metadataPath = Join-Path $appDirectory 'specforge_metadata.json'
    if (Test-Path -LiteralPath $metadataPath -PathType Leaf) {
        $metadata = Get-Content -Raw -LiteralPath $metadataPath |
            ConvertFrom-Json
        $deploymentProperty =
            $metadata.PSObject.Properties['deployment']
        $storageProfileProperty = $null
        if ($null -ne $deploymentProperty -and
            $null -ne $deploymentProperty.Value) {
            $storageProfileProperty =
                $deploymentProperty.Value.PSObject.Properties[
                    'storage_profile']
        }
        if ($null -ne $storageProfileProperty -and
            [string]$storageProfileProperty.Value -eq 'portable') {
            return Join-Path $appDirectory 'Data'
        }
    }
    return Join-Path $env:LOCALAPPDATA 'SpecForge'
}

function Get-LauncherProcessId {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Output
    )

    foreach ($line in $Output) {
        if ([string]$line -match '^SpecForge PID: ([0-9]+)$') {
            return [int]$Matches[1]
        }
    }
    throw (
        'Launcher output did not contain the owned GUI process ID. Output: ' +
        ($Output -join [Environment]::NewLine))
}

$resolvedLauncher = (Resolve-Path -LiteralPath $Launcher).Path
$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$resolvedCleanupFixture =
    (Resolve-Path -LiteralPath $CleanupFixture).Path

$fixtureParent = Join-Path `
    ([System.IO.Path]::GetTempPath()) `
    ('specforge-automation-launcher-tests-' +
        [Guid]::NewGuid().ToString('N'))
$portableAppRoot =
    Join-Path $fixtureParent 'portable-app'
$fixtureExecutable =
    Join-Path `
        $portableAppRoot `
        ([System.IO.Path]::GetFileName(
            $resolvedExecutable))
$stateRoot = Join-Path $fixtureParent 'state'
[System.IO.Directory]::CreateDirectory($fixtureParent) | Out-Null
$incompatibleEnvironmentVariables = @(
    'SPECFORGE_PROFILE',
    'SPECFORGE_PROFILE_DIR',
    'SPECFORGE_RUNTIME_RESOURCE_WORKLOAD',
    'SPECFORGE_RUNTIME_RESOURCE_STATE_DIR'
)
$originalAutomationEnvironment = @{}
foreach ($name in $incompatibleEnvironmentVariables) {
    $originalAutomationEnvironment[$name] =
        [System.Environment]::GetEnvironmentVariable(
            $name,
            [System.EnvironmentVariableTarget]::Process)
}
$cleanupFixtureEnvironment =
    'SPECFORGE_AUTOMATION_LAUNCHER_FIXTURE'
$originalCleanupFixtureEnvironment =
    [System.Environment]::GetEnvironmentVariable(
        $cleanupFixtureEnvironment,
        [System.EnvironmentVariableTarget]::Process)
$bystander = $null

try {
    [System.IO.Directory]::CreateDirectory(
        $portableAppRoot) | Out-Null
    Copy-Item `
        -LiteralPath $resolvedExecutable `
        -Destination $fixtureExecutable
    $buildRoot =
        Split-Path -Parent $resolvedExecutable
    $runtimeDll = Join-Path $buildRoot 'zd.dll'
    if (Test-Path -LiteralPath $runtimeDll -PathType Leaf) {
        Copy-Item `
            -LiteralPath $runtimeDll `
            -Destination (
                Join-Path $portableAppRoot 'zd.dll')
    }
    $runtimeConfig = Join-Path $buildRoot 'config'
    if (Test-Path -LiteralPath $runtimeConfig -PathType Container) {
        Copy-Item `
            -LiteralPath $runtimeConfig `
            -Destination (
                Join-Path $portableAppRoot 'config') `
            -Recurse
    }
    $metadataPath =
        Join-Path $buildRoot 'specforge_metadata.json'
    $portableMetadata =
        Get-Content -Raw -LiteralPath $metadataPath |
            ConvertFrom-Json
    $portableMetadata |
        Add-Member `
            -NotePropertyName deployment `
            -NotePropertyValue ([pscustomobject][ordered]@{
                distribution = 'portable'
                storage_profile = 'portable'
            }) `
            -Force
    [System.IO.File]::WriteAllText(
        (Join-Path $portableAppRoot 'specforge_metadata.json'),
        ($portableMetadata | ConvertTo-Json -Depth 10),
        [System.Text.UTF8Encoding]::new($false))

    $ordinaryRoot =
        Get-OrdinaryStateRoot -AppPath $fixtureExecutable
    Assert-True `
        -Condition (
            $ordinaryRoot -eq (
                Join-Path $portableAppRoot 'Data')) `
        -Message 'Integration fixture requires Portable user-state resolution.'
    [System.IO.Directory]::CreateDirectory($ordinaryRoot) |
        Out-Null

    $ordinarySource =
        Join-Path $fixtureParent 'ordinary-session-marker.csv'
    [System.IO.File]::WriteAllText(
        $ordinarySource,
        "wavelength,flux`n5000,1`n5001,2`n",
        [System.Text.UTF8Encoding]::new($false))
    $ordinarySession = [ordered]@{
        format_kind = 'specforge.source_collection_session.cache'
        schema_version = 2
        active_source_index = 0
        sources = @(
            [ordered]@{
                path = $ordinarySource
                last_index = 0
            }
        )
    } | ConvertTo-Json -Depth 5
    [System.IO.File]::WriteAllText(
        (Join-Path $ordinaryRoot 'source-session.json'),
        $ordinarySession,
        [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText(
        (Join-Path $ordinaryRoot 'ui-language.json'),
        '{"format_kind":"specforge.ui_language.settings","schema_version":1,"language":"zh-Hans"}',
        [System.Text.UTF8Encoding]::new($false))
    $ordinaryBefore =
        Get-TreeFingerprint -Path $ordinaryRoot

    $externalSentinelRoot =
        Join-Path $fixtureParent 'poisoned-external-output'
    [System.IO.Directory]::CreateDirectory(
        $externalSentinelRoot) | Out-Null
    [System.IO.File]::WriteAllText(
        (Join-Path $externalSentinelRoot 'sentinel.txt'),
        'must remain unchanged',
        [System.Text.UTF8Encoding]::new($false))
    $poisonedWorkload =
        Join-Path $externalSentinelRoot 'workload.json'
    [System.IO.File]::WriteAllText(
        $poisonedWorkload,
        '{"poisoned":true}',
        [System.Text.UTF8Encoding]::new($false))
    $externalBefore =
        Get-TreeFingerprint -Path $externalSentinelRoot
    [System.Environment]::SetEnvironmentVariable(
        'SPECFORGE_PROFILE',
        '1',
        [System.EnvironmentVariableTarget]::Process)
    [System.Environment]::SetEnvironmentVariable(
        'SPECFORGE_PROFILE_DIR',
        $externalSentinelRoot,
        [System.EnvironmentVariableTarget]::Process)
    [System.Environment]::SetEnvironmentVariable(
        'SPECFORGE_RUNTIME_RESOURCE_WORKLOAD',
        $poisonedWorkload,
        [System.EnvironmentVariableTarget]::Process)
    [System.Environment]::SetEnvironmentVariable(
        'SPECFORGE_RUNTIME_RESOURCE_STATE_DIR',
        $externalSentinelRoot,
        [System.EnvironmentVariableTarget]::Process)

    $output = @(
        @('wait idle', 'state get', 'app quit') |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $stateRoot 2>&1
    )
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw (
            "Automation launcher exited with $exitCode.`n" +
            ($output -join [Environment]::NewLine))
    }

    $messages = @(
        $output |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    Assert-True `
        -Condition ($messages.Count -eq 7) `
        -Message 'Launcher should emit hello plus accepted/completed pairs for three commands.'

    $hello = $messages[0]
    Assert-True `
        -Condition (
            [string]$hello.type -eq 'hello' -and
            [string]$hello.status -eq 'completed' -and
            [int]$hello.protocol_version -eq 1 -and
            @($hello.capabilities).Count -eq 3 -and
            [int]$hello.max_message_bytes -eq 65536 -and
            [int]$hello.queue_capacity -eq 32) `
        -Message 'Hello should expose the fixed protocol limits and P0 capabilities.'

    $waitAccepted = $messages[1]
    $waitCompleted = $messages[2]
    $stateAccepted = $messages[3]
    $stateCompleted = $messages[4]
    $quitAccepted = $messages[5]
    $quitCompleted = $messages[6]
    $sourcePathProperty =
        $stateCompleted.state.source.PSObject.Properties['path']
    $ordinarySourceNotImported =
        $null -eq $sourcePathProperty -or
        [string]$sourcePathProperty.Value -ne $ordinarySource
    Assert-True `
        -Condition (
            [string]$stateAccepted.command -eq 'state.get' -and
            [string]$stateAccepted.status -eq 'accepted' -and
            [string]$stateCompleted.status -eq 'completed' -and
            [bool]$stateCompleted.state.shell.idle -and
            [bool]$stateCompleted.state.shell.source_load_idle -and
            [bool]$stateCompleted.state.shell.pending_completion_idle -and
            [bool]$stateCompleted.state.shell.background_retirement_idle -and
            [bool]$stateCompleted.state.window.visible -and
            [bool]$stateCompleted.state.runtime.running -and
            $ordinarySourceNotImported) `
        -Message 'state.get should return the stable instance, queue, Shell, source, window and runtime contract.'
    Assert-True `
        -Condition (
            [string]$waitAccepted.command -eq 'wait.idle' -and
            [string]$waitAccepted.status -eq 'accepted' -and
            [string]$waitCompleted.status -eq 'completed') `
        -Message 'wait.idle should complete after prior controls and Shell activity are idle.'
    Assert-True `
        -Condition (
            [string]$quitAccepted.command -eq 'app.quit' -and
            [string]$quitAccepted.status -eq 'accepted' -and
            [string]$quitCompleted.status -eq 'completed') `
        -Message 'app.quit should complete through the normal process shutdown path.'

    Assert-True `
        -Condition (
            (Test-Path -LiteralPath $stateRoot -PathType Container) -and
            -not (Test-Path `
                -LiteralPath (
                    Join-Path $stateRoot 'automation-startup-error.txt') `
                -PathType Leaf)) `
        -Message 'Launcher should create a clean isolated state root with no startup failure.'

    $ordinaryAfter = Get-TreeFingerprint -Path $ordinaryRoot
    Assert-True `
        -Condition ($ordinaryAfter -ceq $ordinaryBefore) `
        -Message 'Real automation startup, state, idle and quit must not read/import or modify ordinary user state.'

    $externalAfter =
        Get-TreeFingerprint -Path $externalSentinelRoot
    Assert-True `
        -Condition ($externalAfter -ceq $externalBefore) `
        -Message 'Launcher must strip inherited legacy workload/profile variables before starting automation.'

    $invalidNestedStateRoot =
        Join-Path $ordinaryRoot 'automation-child'
    $ordinaryBeforeRejectedLaunch =
        Get-TreeFingerprint -Path $ordinaryRoot
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $rejectedOutput = @(
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $invalidNestedStateRoot 2>&1
        )
        $rejectedExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    Assert-True `
        -Condition (
            $rejectedExitCode -ne 0 -and
            -not (Test-Path -LiteralPath $invalidNestedStateRoot) -and
            (Get-TreeFingerprint -Path $ordinaryRoot) -ceq
                $ordinaryBeforeRejectedLaunch) `
        -Message (
            'Launcher must reject an ordinary-root child before creation or diagnostic writes. Output: ' +
            ($rejectedOutput -join [Environment]::NewLine))

    $bystander = Start-Process `
        -FilePath (Join-Path $PSHOME 'powershell.exe') `
        -ArgumentList @(
            '-NoProfile',
            '-Command',
            'Start-Sleep -Seconds 60'
        ) `
        -WindowStyle Hidden `
        -PassThru

    $gracefulCleanupRoot =
        Join-Path $fixtureParent 'cleanup-graceful-state'
    [System.Environment]::SetEnvironmentVariable(
        $cleanupFixtureEnvironment,
        'graceful',
        [System.EnvironmentVariableTarget]::Process)
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $gracefulCleanupOutput = @(
            'state get' |
                & $resolvedLauncher `
                    --app $resolvedCleanupFixture `
                    --state-root $gracefulCleanupRoot 2>&1
        )
        $gracefulCleanupExitCode =
            $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    $gracefulCleanupPid =
        Get-LauncherProcessId `
            -Output $gracefulCleanupOutput
    Assert-True `
        -Condition (
            $gracefulCleanupExitCode -ne 0 -and
            $null -eq (
                Get-Process `
                    -Id $gracefulCleanupPid `
                    -ErrorAction SilentlyContinue) -and
            (Test-Path `
                -LiteralPath (
                    Join-Path `
                        $gracefulCleanupRoot `
                        'launcher-cleanup-completed.txt') `
                -PathType Leaf)) `
        -Message (
            'A post-handshake launcher error must request normal app.quit and leave no owned process. Output: ' +
            ($gracefulCleanupOutput -join [Environment]::NewLine))

    $forcedCleanupRoot =
        Join-Path $fixtureParent 'cleanup-forced-state'
    [System.Environment]::SetEnvironmentVariable(
        $cleanupFixtureEnvironment,
        'disconnect',
        [System.EnvironmentVariableTarget]::Process)
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $forcedCleanupOutput = @(
            'state get' |
                & $resolvedLauncher `
                    --app $resolvedCleanupFixture `
                    --state-root $forcedCleanupRoot 2>&1
        )
        $forcedCleanupExitCode =
            $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    $forcedCleanupPid =
        Get-LauncherProcessId `
            -Output $forcedCleanupOutput
    Assert-True `
        -Condition (
            $forcedCleanupExitCode -ne 0 -and
            $null -eq (
                Get-Process `
                    -Id $forcedCleanupPid `
                    -ErrorAction SilentlyContinue) -and
            -not (Test-Path `
                -LiteralPath (
                    Join-Path `
                        $forcedCleanupRoot `
                        'launcher-cleanup-completed.txt') `
                -PathType Leaf)) `
        -Message (
            'A disconnected launcher-owned child must be terminated within the cleanup bound. Output: ' +
            ($forcedCleanupOutput -join [Environment]::NewLine))

    $bystander.Refresh()
    Assert-True `
        -Condition (-not $bystander.HasExited) `
        -Message 'Launcher cleanup must not affect a non-owned bystander process.'
}
finally {
    foreach ($name in $incompatibleEnvironmentVariables) {
        [System.Environment]::SetEnvironmentVariable(
            $name,
            $originalAutomationEnvironment[$name],
            [System.EnvironmentVariableTarget]::Process)
    }
    [System.Environment]::SetEnvironmentVariable(
        $cleanupFixtureEnvironment,
        $originalCleanupFixtureEnvironment,
        [System.EnvironmentVariableTarget]::Process)
    if ($null -ne $bystander) {
        $bystander.Refresh()
        if (-not $bystander.HasExited) {
            Stop-Process `
                -Id $bystander.Id `
                -Force
            [void]$bystander.WaitForExit(5000)
        }
        $bystander.Dispose()
    }
    if (Test-Path -LiteralPath $fixtureParent) {
        $resolvedFixture = (Resolve-Path -LiteralPath $fixtureParent).Path
        $tempRoot = [System.IO.Path]::GetFullPath(
            [System.IO.Path]::GetTempPath())
        if (-not $resolvedFixture.StartsWith(
                $tempRoot,
                [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to remove fixture outside temp: $resolvedFixture"
        }
        Remove-Item `
            -LiteralPath $resolvedFixture `
            -Recurse `
            -Force
    }
}

Write-Host 'automation launcher integration tests passed'
