[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Launcher,
    [Parameter(Mandatory = $true)]
    [string]$Executable,
    [Parameter(Mandatory = $true)]
    [string]$CleanupFixture,
    [Parameter(Mandatory = $true)]
    [string]$StateFixture
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

function Get-PngIhdr {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $signature = [byte[]]@(
        0x89, 0x50, 0x4E, 0x47,
        0x0D, 0x0A, 0x1A, 0x0A)
    Assert-True `
        -Condition (
            $bytes.Length -ge 24 -and
            -not (
                Compare-Object `
                    -ReferenceObject $signature `
                    -DifferenceObject $bytes[0..7])) `
        -Message 'Captured output should have a complete PNG signature and IHDR.'
    Assert-True `
        -Condition (
            [char]$bytes[12] -eq 'I' -and
            [char]$bytes[13] -eq 'H' -and
            [char]$bytes[14] -eq 'D' -and
            [char]$bytes[15] -eq 'R') `
        -Message 'Captured output should begin with a PNG IHDR chunk.'
    $width =
        ([uint32]$bytes[16] -shl 24) -bor
        ([uint32]$bytes[17] -shl 16) -bor
        ([uint32]$bytes[18] -shl 8) -bor
        [uint32]$bytes[19]
    $height =
        ([uint32]$bytes[20] -shl 24) -bor
        ([uint32]$bytes[21] -shl 16) -bor
        ([uint32]$bytes[22] -shl 8) -bor
        [uint32]$bytes[23]
    return [pscustomobject]@{
        Width = $width
        Height = $height
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

function Get-ProtocolMessage {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Messages,
        [Parameter(Mandatory = $true)]
        [string]$RequestId,
        [Parameter(Mandatory = $true)]
        [string]$Status
    )

    $selected = @(
        $Messages |
            Where-Object {
                [string]$_.request_id -eq $RequestId -and
                [string]$_.status -eq $Status
            }
    )
    if ($selected.Count -ne 1) {
        throw (
            "Expected one $Status message for $RequestId, found " +
            $selected.Count +
            '. Messages: ' +
            ($Messages |
                ConvertTo-Json -Compress -Depth 20))
    }
    return $selected[0]
}

function Get-ProtocolTerminalMessage {
    param(
        [Parameter(Mandatory = $true)]
        [object[]]$Messages,
        [Parameter(Mandatory = $true)]
        [string]$RequestId
    )

    $selected = @(
        $Messages |
            Where-Object {
                [string]$_.request_id -eq $RequestId -and
                [string]$_.status -in @('completed', 'failed')
            }
    )
    if ($selected.Count -ne 1) {
        throw (
            "Expected one terminal message for $RequestId, found " +
            $selected.Count +
            '. Messages: ' +
            ($Messages |
                ConvertTo-Json -Compress -Depth 20))
    }
    return $selected[0]
}

function Read-LauncherLine {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,
        [int]$TimeoutMilliseconds = 10000
    )

    $read = $Process.StandardOutput.ReadLineAsync()
    if (-not $read.Wait($TimeoutMilliseconds)) {
        throw 'Timed out waiting for interactive launcher output.'
    }
    if ($null -eq $read.Result) {
        throw (
            'Interactive launcher closed stdout unexpectedly with exit code ' +
            $(if ($Process.HasExited) {
                $Process.ExitCode
            }
            else {
                'still-running'
            }))
    }
    return [string]$read.Result
}

function Send-InteractiveLauncherRequest {
    param(
        [Parameter(Mandatory = $true)]
        [System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)]
        [string[]]$Lines,
        [Parameter(Mandatory = $true)]
        [string]$RequestId
    )

    $payload =
        [System.Text.UTF8Encoding]::new($false).GetBytes(
            ($Lines -join "`n") + "`n")
    $inputStream =
        $Process.StandardInput.BaseStream
    $inputStream.Write(
        $payload,
        0,
        $payload.Length)
    $inputStream.Flush()
    $messages = @()
    while ($true) {
        $line =
            Read-LauncherLine -Process $Process
        if ($line -notmatch '^\{') {
            continue
        }
        $message = $line | ConvertFrom-Json
        if ([string]$message.request_id -ne $RequestId) {
            throw (
                "Interactive launcher returned an unexpected request ID: $line")
        }
        $messages += $message
        if ([string]$message.status -in @(
                'completed',
                'failed',
                'canceled')) {
            break
        }
    }
    Assert-True `
        -Condition (
            $messages.Count -eq 2 -and
            [string]$messages[0].status -eq 'accepted') `
        -Message "Interactive request $RequestId should emit accepted and one terminal."
    return $messages
}

if ($null -eq (
        'SpecForgeAutomationWindowTestNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class SpecForgeAutomationWindowTestNative
{
    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool ShowWindowAsync(
        IntPtr window,
        int command);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool IsWindowVisible(IntPtr window);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool IsIconic(IntPtr window);

    [DllImport(
        "kernel32.dll",
        CharSet = CharSet.Unicode,
        SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool MoveFile(
        string existingPath,
        string newPath);
}
'@
}

function Invoke-RejectedGuiWorkflow {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Commands,
        [Parameter(Mandatory = $true)]
        [string]$LauncherPath,
        [Parameter(Mandatory = $true)]
        [string]$AppPath,
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [Parameter(Mandatory = $true)]
        [string]$Seed
    )

    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(
            $Commands |
                & $LauncherPath `
                    --app $AppPath `
                    --state-root $Root `
                    --labeling-state-seed $Seed 2>&1
        )
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    $messages = @(
        $output |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = $output
        Messages = $messages
        Pid = Get-LauncherProcessId -Output $output
    }
}

$resolvedLauncher = (Resolve-Path -LiteralPath $Launcher).Path
$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$resolvedCleanupFixture =
    (Resolve-Path -LiteralPath $CleanupFixture).Path
$resolvedStateFixture =
    (Resolve-Path -LiteralPath $StateFixture).Path

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

    $sourceRoot = Join-Path $fixtureParent 'source-fixture'
    [System.IO.Directory]::CreateDirectory(
        $sourceRoot) | Out-Null
    foreach ($name in @(
            'alpha.csv',
            'bravo.csv',
            'charlie.csv')) {
        [System.IO.File]::WriteAllText(
            (Join-Path $sourceRoot $name),
            "wavelength,flux`n5000,1`n5001,2`n",
            [System.Text.UTF8Encoding]::new($false))
    }
    $labelSeed =
        Join-Path $fixtureParent 'labeling-seed.json'
    & $resolvedStateFixture `
        --write-labeling-seed $labelSeed `
        --source $sourceRoot
    if ($LASTEXITCODE -ne 0) {
        throw (
            'Production labeling fixture generation failed with exit code ' +
            $LASTEXITCODE)
    }
    $seedBefore =
        Get-FileSha256 -Path $labelSeed
    $capturePath =
        Join-Path $stateRoot 'artifacts\labeled-spectrum.png'

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
        @(
            "source open $sourceRoot",
            'wait idle',
            'spectrum goto 2',
            'label assign 5 spectrum 1',
            'spectrum goto 1',
            'wait idle',
            "frame capture $capturePath",
            'state get',
            'app quit'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $stateRoot `
                --labeling-state-seed $labelSeed 2>&1
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
        -Condition ($messages.Count -eq 19) `
        -Message 'Launcher should emit hello plus accepted/completed pairs for the nine-command GUI workflow.'

    $hello = $messages[0]
    Assert-True `
        -Condition (
            [string]$hello.type -eq 'hello' -and
            [string]$hello.status -eq 'completed' -and
            [int]$hello.protocol_version -eq 1 -and
            @($hello.capabilities).Count -eq 7 -and
            [int]$hello.max_message_bytes -eq 65536 -and
            [int]$hello.queue_capacity -eq 32) `
        -Message 'Hello should expose the fixed protocol limits and P0 capabilities.'

    $sourceAccepted = $messages[1]
    $sourceCompleted = $messages[2]
    $waitAccepted = $messages[3]
    $waitCompleted = $messages[4]
    $gotoAccepted = $messages[5]
    $gotoCompleted = $messages[6]
    $labelAccepted = $messages[7]
    $labelCompleted = $messages[8]
    $returnAccepted = $messages[9]
    $returnCompleted = $messages[10]
    $secondWaitAccepted = $messages[11]
    $secondWaitCompleted = $messages[12]
    $captureAccepted = $messages[13]
    $captureCompleted = $messages[14]
    $stateAccepted = $messages[15]
    $stateCompleted = $messages[16]
    $quitAccepted = $messages[17]
    $quitCompleted = $messages[18]
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
            -not [bool]$stateCompleted.state.window.minimized -and
            [bool]$stateCompleted.state.runtime.running -and
            [bool]$stateCompleted.state.source.present -and
            [string]$stateCompleted.state.source.path -eq $sourceRoot -and
            [bool]$stateCompleted.state.presented_source.present -and
            [string]$stateCompleted.state.presented_source.path -eq
                $sourceRoot -and
            [string]$stateCompleted.state.presented_source.id -eq
                [string]$stateCompleted.state.source.id -and
            [int]$stateCompleted.state.spectrum.index -eq 1 -and
            [string]$stateCompleted.state.spectrum.name -eq 'bravo.csv' -and
            [int]$stateCompleted.state.labeling.current_spectrum_label.code -eq 5 -and
            -not [bool]$stateCompleted.state.capture.pending -and
            [string]$stateCompleted.state.capture.last_result -eq 'succeeded' -and
            [string]$stateCompleted.state.capture.last_path -eq $capturePath -and
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
            [string]$sourceAccepted.command -eq 'source.open' -and
            [string]$sourceCompleted.status -eq 'completed' -and
            [int]$sourceCompleted.result.source.spectrum_count -eq 3 -and
            [string]$gotoCompleted.status -eq 'completed' -and
            [int]$gotoCompleted.result.spectrum.index -eq 2 -and
            [string]$labelAccepted.command -eq 'label.assign' -and
            [string]$labelCompleted.status -eq 'completed' -and
            [int]$labelCompleted.result.assignment.spectrum.index -eq 1 -and
            [int]$labelCompleted.result.assignment.previous_code -eq -1 -and
            [int]$labelCompleted.result.assignment.new_code -eq 5 -and
            [bool]$labelCompleted.result.assignment.changed -and
            [string]$labelCompleted.result.persistence.status -eq
                'state_save_scheduled' -and
            [bool]$labelCompleted.result.persistence.state_save_scheduled -and
            -not [bool]$labelCompleted.result.persistence.state_save_attempted -and
            [int]$labelCompleted.result.current_spectrum_after.index -eq 2 -and
            [string]$returnAccepted.command -eq 'spectrum.goto' -and
            [string]$returnCompleted.status -eq 'completed' -and
            [int]$returnCompleted.result.spectrum.index -eq 1 -and
            [string]$secondWaitAccepted.command -eq 'wait.idle' -and
            [string]$secondWaitCompleted.status -eq 'completed') `
        -Message 'Source, navigation and auto-advancing label assignment should complete through the real session seam.'
    Assert-True `
        -Condition (
            [string]$captureAccepted.command -eq 'frame.capture' -and
            [string]$captureCompleted.status -eq 'completed' -and
            [string]$captureCompleted.result.path -eq $capturePath -and
            [string]$captureCompleted.result.format -eq 'png' -and
            [string]$captureCompleted.result.scope -eq 'main_viewport' -and
            [int]$captureCompleted.result.width -gt 0 -and
            [int]$captureCompleted.result.height -gt 0) `
        -Message 'frame.capture should report the requested in-root application-rendered PNG.'
    Assert-True `
        -Condition (
            [string]$quitAccepted.command -eq 'app.quit' -and
            [string]$quitAccepted.status -eq 'accepted' -and
            [string]$quitCompleted.status -eq 'completed') `
        -Message 'app.quit should complete through the normal process shutdown path.'

    Assert-True `
        -Condition (
            (Test-Path -LiteralPath $capturePath -PathType Leaf) -and
            (Get-Item -LiteralPath $capturePath).Length -gt 8) `
        -Message 'Captured PNG should exist and be non-empty.'
    $pngIhdr = Get-PngIhdr -Path $capturePath
    Assert-True `
        -Condition (
            [uint32]$pngIhdr.Width -eq
                [uint32]$captureCompleted.result.width -and
            [uint32]$pngIhdr.Height -eq
                [uint32]$captureCompleted.result.height) `
        -Message 'Captured PNG IHDR dimensions must match the terminal dimensions from the application render target.'

    & $resolvedStateFixture `
        --verify-labeling-state (
            Join-Path $stateRoot 'sample-labeling-tasks.json') `
        --source $sourceRoot `
        --spectrum-index 1 `
        --expected-code 5
    $productionReloadExitCode = $LASTEXITCODE
    Assert-True `
        -Condition ($productionReloadExitCode -eq 0) `
        -Message 'Normal app.quit should persist the assigned label and the production loader should reload the expected source-row value.'
    Assert-True `
        -Condition (
            (Get-FileSha256 -Path $labelSeed) -ceq $seedBefore) `
        -Message 'Launcher-owned materialization must not modify the read-only seed fixture.'

    Assert-True `
        -Condition (
            (Test-Path -LiteralPath $stateRoot -PathType Container) -and
            -not (Test-Path `
                -LiteralPath (
                    Join-Path $stateRoot 'automation-startup-error.txt') `
                -PathType Leaf)) `
        -Message 'Launcher should create a clean isolated state root with no startup failure.'

    $guiPid = Get-LauncherProcessId -Output $output
    Assert-True `
        -Condition (
            $null -eq (
                Get-Process `
                    -Id $guiPid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'Normal app.quit should leave no GUI process.'

    $windowContractRoot =
        Join-Path $fixtureParent 'window-contract-state'
    $visibleCapturePath =
        Join-Path $windowContractRoot 'captures\visible.png'
    $hiddenCapturePath =
        Join-Path $windowContractRoot 'captures\hidden.png'
    $minimizedCapturePath =
        Join-Path $windowContractRoot 'captures\minimized.png'
    $unicodeSourceRoot =
        Join-Path $fixtureParent (
            [string][char]0x6570 +
            [string][char]0x636E +
            [string][char]0x6E90)
    [System.IO.Directory]::CreateDirectory(
        $unicodeSourceRoot) | Out-Null
    $unicodeFirstSpectrum =
        [string][char]0x5149 +
        [string][char]0x8C31 +
        [string][char]0x7532 +
        '.csv'
    $unicodeSecondSpectrum =
        [string][char]0x5149 +
        [string][char]0x8C31 +
        [string][char]0x4E59 +
        '.csv'
    foreach ($name in @(
            $unicodeFirstSpectrum,
            $unicodeSecondSpectrum)) {
        [System.IO.File]::WriteAllText(
            (Join-Path $unicodeSourceRoot $name),
            "wavelength,flux`n5000,1`n5001,2`n",
            [System.Text.UTF8Encoding]::new($false))
    }
    $interactiveLauncher = $null
    $interactiveGuiPid = 0
    $foregroundBefore =
        [SpecForgeAutomationWindowTestNative]::
            GetForegroundWindow()
    try {
        $start = [System.Diagnostics.ProcessStartInfo]::new()
        $start.FileName = $resolvedLauncher
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardInput = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $utf8WithoutBom =
            [System.Text.UTF8Encoding]::new($false)
        $start.StandardOutputEncoding =
            $utf8WithoutBom
        $start.StandardErrorEncoding =
            $utf8WithoutBom
        $start.Arguments =
            '--app "' + $fixtureExecutable +
            '" --state-root "' +
            $windowContractRoot + '"'
        $interactiveLauncher =
            [System.Diagnostics.Process]::Start($start)

        $initialOutput = @()
        while ($true) {
            $line =
                Read-LauncherLine `
                    -Process $interactiveLauncher
            $initialOutput += $line
            if ($line -match '^SpecForge PID: ([0-9]+)$') {
                $interactiveGuiPid =
                    [int]$Matches[1]
            }
            if ($line -eq (
                    'Harness controls: pipeline begin ... pipeline end; ' +
                    'disconnect after accepted <next command>')) {
                break
            }
        }
        Assert-True `
            -Condition ($interactiveGuiPid -gt 0) `
            -Message 'Interactive launcher should report its owned GUI PID.'

        $windowHandle = [IntPtr]::Zero
        $windowDeadline =
            [DateTime]::UtcNow.AddSeconds(10)
        while ([DateTime]::UtcNow -lt $windowDeadline) {
            $guiProcess =
                Get-Process `
                    -Id $interactiveGuiPid `
                    -ErrorAction SilentlyContinue
            if ($null -ne $guiProcess) {
                $guiProcess.Refresh()
                $windowHandle =
                    [IntPtr]$guiProcess.MainWindowHandle
            }
            if ($windowHandle -ne [IntPtr]::Zero -and
                [SpecForgeAutomationWindowTestNative]::
                    IsWindowVisible($windowHandle)) {
                break
            }
            Start-Sleep -Milliseconds 25
        }
        $foregroundAfterLaunch =
            [SpecForgeAutomationWindowTestNative]::
                GetForegroundWindow()
        Assert-True `
            -Condition (
                $windowHandle -ne [IntPtr]::Zero -and
                [SpecForgeAutomationWindowTestNative]::
                    IsWindowVisible($windowHandle) -and
                -not [SpecForgeAutomationWindowTestNative]::
                    IsIconic($windowHandle) -and
                $foregroundAfterLaunch -ne $windowHandle -and
                ($foregroundBefore -eq [IntPtr]::Zero -or
                 $foregroundAfterLaunch -eq
                    $foregroundBefore)) `
            -Message 'The real SpecForge HWND must become visible and renderable without activation or foreground theft.'

        $movedWindowRoot =
            Join-Path $fixtureParent 'window-contract-state-moved'
        $rootMoved =
            [SpecForgeAutomationWindowTestNative]::
                MoveFile(
                    $windowContractRoot,
                    $movedWindowRoot)
        $rootMoveError =
            [Runtime.InteropServices.Marshal]::
                GetLastWin32Error()
        Assert-True `
            -Condition (
                -not $rootMoved -and
                $rootMoveError -in @(5, 32, 33) -and
                (Test-Path `
                    -LiteralPath $windowContractRoot `
                    -PathType Container) -and
                -not (Test-Path `
                    -LiteralPath $movedWindowRoot)) `
            -Message 'The launcher-owned identity lease must prevent state-root replacement for the complete GUI child lifetime.'

        $unicodeOpenMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @(
                    "source open $unicodeSourceRoot") `
                -RequestId 'request-1'
        )
        $unicodeWaitMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @('wait idle') `
                -RequestId 'request-2'
        )
        $unicodeGotoMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @(
                    "spectrum goto name $unicodeSecondSpectrum") `
                -RequestId 'request-3'
        )
        $unicodeStateMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @('state get') `
                -RequestId 'request-4'
        )
        $unicodeStateTerminal =
            $unicodeStateMessages[1]
        Assert-True `
            -Condition (
                [string]$unicodeOpenMessages[1].status -eq
                    'completed' -and
                [string]$unicodeWaitMessages[1].status -eq
                    'completed' -and
                [string]$unicodeGotoMessages[1].status -eq
                    'completed' -and
                [string]$unicodeGotoMessages[1].result.spectrum.name -eq
                    $unicodeSecondSpectrum -and
                [string]$unicodeStateTerminal.state.source.path -eq
                    $unicodeSourceRoot -and
                [string]$unicodeStateTerminal.state.presented_source.path -eq
                    $unicodeSourceRoot -and
                [string]$unicodeStateTerminal.state.spectrum.name -eq
                    $unicodeSecondSpectrum) `
            -Message 'Explicit UTF-8 redirected stdin must preserve a Unicode source path and exact Unicode spectrum name through the real GUI protocol.'

        $visibleCaptureMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @(
                    "frame capture $visibleCapturePath") `
                -RequestId 'request-5'
        )
        $visibleCaptureTerminal =
            $visibleCaptureMessages[1]
        $visibleIhdr =
            Get-PngIhdr -Path $visibleCapturePath
        Assert-True `
            -Condition (
                [string]$visibleCaptureTerminal.status -eq
                    'completed' -and
                [uint32]$visibleIhdr.Width -eq
                    [uint32]$visibleCaptureTerminal.result.width -and
                [uint32]$visibleIhdr.Height -eq
                    [uint32]$visibleCaptureTerminal.result.height -and
                [SpecForgeAutomationWindowTestNative]::
                    GetForegroundWindow() -ne $windowHandle) `
            -Message 'A visible but non-activated real HWND should produce an application-rendered PNG whose IHDR matches its terminal.'

        [void][SpecForgeAutomationWindowTestNative]::
            ShowWindowAsync($windowHandle, 0)
        $visibilityDeadline =
            [DateTime]::UtcNow.AddSeconds(5)
        while (
            [SpecForgeAutomationWindowTestNative]::
                IsWindowVisible($windowHandle) -and
            [DateTime]::UtcNow -lt $visibilityDeadline) {
            Start-Sleep -Milliseconds 25
        }
        $hiddenCaptureMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @(
                    'pipeline begin',
                    "frame capture $hiddenCapturePath",
                    'pipeline end') `
                -RequestId 'request-6'
        )
        Assert-True `
            -Condition (
                -not [SpecForgeAutomationWindowTestNative]::
                    IsWindowVisible($windowHandle) -and
                [string]$hiddenCaptureMessages[1].status -eq
                    'failed' -and
                [string]$hiddenCaptureMessages[1].error.code -eq
                    'window_not_renderable' -and
                -not (Test-Path -LiteralPath $hiddenCapturePath) -and
                [SpecForgeAutomationWindowTestNative]::
                    GetForegroundWindow() -ne $windowHandle) `
            -Message 'A hidden real HWND must fail capture without restoring, activating, or publishing a PNG.'

        [void][SpecForgeAutomationWindowTestNative]::
            ShowWindowAsync($windowHandle, 4)
        $restoreDeadline =
            [DateTime]::UtcNow.AddSeconds(5)
        while (
            (-not [SpecForgeAutomationWindowTestNative]::
                IsWindowVisible($windowHandle) -or
             [SpecForgeAutomationWindowTestNative]::
                IsIconic($windowHandle)) -and
            [DateTime]::UtcNow -lt $restoreDeadline) {
            Start-Sleep -Milliseconds 25
        }
        [void][SpecForgeAutomationWindowTestNative]::
            ShowWindowAsync($windowHandle, 7)
        $minimizeDeadline =
            [DateTime]::UtcNow.AddSeconds(5)
        while (
            -not [SpecForgeAutomationWindowTestNative]::
                IsIconic($windowHandle) -and
            [DateTime]::UtcNow -lt $minimizeDeadline) {
            Start-Sleep -Milliseconds 25
        }
        $minimizedCaptureMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @(
                    'pipeline begin',
                    "frame capture $minimizedCapturePath",
                    'pipeline end') `
                -RequestId 'request-7'
        )
        Assert-True `
            -Condition (
                [SpecForgeAutomationWindowTestNative]::
                    IsIconic($windowHandle) -and
                [string]$minimizedCaptureMessages[1].status -eq
                    'failed' -and
                [string]$minimizedCaptureMessages[1].error.code -eq
                    'window_not_renderable' -and
                -not (Test-Path -LiteralPath $minimizedCapturePath) -and
                [SpecForgeAutomationWindowTestNative]::
                    GetForegroundWindow() -ne $windowHandle) `
            -Message 'A minimized real HWND must fail capture without restoring, activating, or publishing a PNG.'

        [void][SpecForgeAutomationWindowTestNative]::
            ShowWindowAsync($windowHandle, 4)
        $quitMessages = @(
            Send-InteractiveLauncherRequest `
                -Process $interactiveLauncher `
                -Lines @('app quit') `
                -RequestId 'request-8'
        )
        Assert-True `
            -Condition (
                [string]$quitMessages[1].status -eq
                    'completed' -and
                $interactiveLauncher.WaitForExit(15000) -and
                $interactiveLauncher.ExitCode -eq 0) `
            -Message 'Interactive HWND contract workflow should quit normally.'
        Assert-True `
            -Condition (
                $null -eq (
                    Get-Process `
                        -Id $interactiveGuiPid `
                        -ErrorAction SilentlyContinue)) `
            -Message 'Interactive HWND contract workflow should leave no GUI process.'
        Assert-True `
            -Condition (
                -not (Test-Path -LiteralPath (
                    Join-Path $windowContractRoot (
                        '.specforge-automation-root.lock')))) `
            -Message 'The launcher must release and remove its state-root identity lock after the owned GUI exits.'
    }
    finally {
        if ($null -ne $interactiveLauncher) {
            if (-not $interactiveLauncher.HasExited) {
                $interactiveLauncher.StandardInput.Close()
                if (-not $interactiveLauncher.WaitForExit(3000)) {
                    $interactiveLauncher.Kill($true)
                    [void]$interactiveLauncher.WaitForExit(3000)
                }
            }
            $interactiveLauncher.Dispose()
        }
        if ($interactiveGuiPid -gt 0) {
            $ownedGui =
                Get-Process `
                    -Id $interactiveGuiPid `
                    -ErrorAction SilentlyContinue
            if ($null -ne $ownedGui) {
                [void]$ownedGui.CloseMainWindow()
                if (-not $ownedGui.WaitForExit(3000)) {
                    Stop-Process `
                        -Id $interactiveGuiPid `
                        -Force `
                        -ErrorAction SilentlyContinue
                }
            }
        }
    }

    $repeatStateRoot =
        Join-Path $fixtureParent 'state-repeat'
    $repeatCapturePath =
        Join-Path $repeatStateRoot 'repeat\labeled.png'
    $repeatOutput = @(
        @(
            "source open $sourceRoot",
            'wait idle',
            'spectrum goto 2',
            'label assign 5 spectrum 1',
            'spectrum goto 1',
            'wait idle',
            "frame capture $repeatCapturePath",
            'state get',
            'app quit'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $repeatStateRoot `
                --labeling-state-seed $labelSeed 2>&1
    )
    $repeatExitCode = $LASTEXITCODE
    if ($repeatExitCode -ne 0) {
        throw (
            "Repeated automation workflow exited with $repeatExitCode.`n" +
            ($repeatOutput -join [Environment]::NewLine))
    }
    $repeatMessages = @(
        $repeatOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $repeatState = $repeatMessages[16]
    Assert-True `
        -Condition (
            $repeatMessages.Count -eq 19 -and
            [string]$repeatMessages[8].status -eq 'completed' -and
            [int]$repeatMessages[8].result.assignment.spectrum.index -eq 1 -and
            [int]$repeatState.state.spectrum.index -eq 1 -and
            [int]$repeatState.state.labeling.current_spectrum_label.code -eq 5 -and
            (Test-Path -LiteralPath $repeatCapturePath -PathType Leaf) -and
            (Get-Item -LiteralPath $repeatCapturePath).Length -gt 8) `
        -Message 'The complete isolated GUI workflow should be stable on a second fresh root.'
    $repeatPid =
        Get-LauncherProcessId -Output $repeatOutput
    Assert-True `
        -Condition (
            $null -eq (
                Get-Process `
                    -Id $repeatPid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'Repeated workflow should leave no GUI process.'

    $signedIndexRoot =
        Join-Path $fixtureParent 'signed-index-state'
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $signedIndexOutput = @(
            @(
                'spectrum goto -1',
                'label assign 5 spectrum -1',
                'app quit'
            ) |
                & $resolvedLauncher `
                    --app $fixtureExecutable `
                    --state-root $signedIndexRoot 2>&1
        )
        $signedIndexExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    $signedIndexMessages = @(
        $signedIndexOutput |
            Where-Object {
                [string]$_ -match '^\{'
            } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $signedIndexPid =
        Get-LauncherProcessId `
            -Output $signedIndexOutput
    Assert-True `
        -Condition (
            $signedIndexExitCode -eq 0 -and
            $signedIndexMessages.Count -eq 3 -and
            [string]$signedIndexMessages[1].command -eq
                'app.quit' -and
            [string]$signedIndexMessages[2].status -eq
                'completed' -and
            @(
                $signedIndexOutput |
                    Where-Object {
                        [string]$_ -match
                            'must be an unsigned decimal integer'
                    }
            ).Count -eq 2 -and
            $null -eq (
                Get-Process `
                    -Id $signedIndexPid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'CLI index parsing must reject signed spectrum targets locally without consuming request IDs, while the owned GUI still quits normally.'

    $sourceRootB =
        Join-Path $fixtureParent 'source-fixture-b'
    [System.IO.Directory]::CreateDirectory(
        $sourceRootB) | Out-Null
    foreach ($name in @('delta.csv', 'echo.csv')) {
        [System.IO.File]::WriteAllText(
            (Join-Path $sourceRootB $name),
            "wavelength,flux`n6000,3`n6001,4`n",
            [System.Text.UTF8Encoding]::new($false))
    }

    $concurrentRoot =
        Join-Path $fixtureParent 'concurrent-open-state'
    $concurrentOutput = @(
        @(
            'pipeline begin',
            "source open $sourceRoot",
            "source open $sourceRootB",
            'wait idle',
            'pipeline end',
            'state get',
            'app quit'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $concurrentRoot `
                --labeling-state-seed $labelSeed 2>&1
    )
    $concurrentExitCode = $LASTEXITCODE
    if ($concurrentExitCode -ne 0) {
        throw (
            "Concurrent source workflow exited with $concurrentExitCode.`n" +
            ($concurrentOutput -join [Environment]::NewLine))
    }
    $concurrentMessages = @(
        $concurrentOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $firstOpenCanceled =
        Get-ProtocolMessage `
            -Messages $concurrentMessages `
            -RequestId 'request-1' `
            -Status 'failed'
    $secondOpenCompleted =
        Get-ProtocolMessage `
            -Messages $concurrentMessages `
            -RequestId 'request-2' `
            -Status 'completed'
    $concurrentWaitCompleted =
        Get-ProtocolMessage `
            -Messages $concurrentMessages `
            -RequestId 'request-3' `
            -Status 'completed'
    $concurrentState =
        Get-ProtocolMessage `
            -Messages $concurrentMessages `
            -RequestId 'request-4' `
            -Status 'completed'
    Assert-True `
        -Condition (
            [string]$firstOpenCanceled.error.code -eq
                'operation_canceled' -and
            [string]$secondOpenCompleted.result.source.path -eq
                $sourceRootB -and
            [string]$concurrentState.state.source.path -eq
                $sourceRootB -and
            [string]$concurrentWaitCompleted.command -eq
                'wait.idle') `
        -Message 'Two pipelined source opens should leave the older request terminal canceled, activate the newer source, and release wait.idle.'
    $concurrentPid =
        Get-LauncherProcessId -Output $concurrentOutput
    Assert-True `
        -Condition (
            $null -eq (
                Get-Process `
                    -Id $concurrentPid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'Concurrent source-open workflow should leave no GUI process.'

    $earlierBarrierRoot =
        Join-Path $fixtureParent 'earlier-wait-barrier-state'
    $earlierBarrierOutput = @(
        @(
            'pipeline begin',
            "source open $sourceRoot",
            'wait idle',
            "source open $sourceRootB",
            'pipeline end',
            'state get',
            'app quit'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $earlierBarrierRoot 2>&1
    )
    if ($LASTEXITCODE -ne 0) {
        throw (
            "Earlier-only wait barrier exited with $LASTEXITCODE.`n" +
            ($earlierBarrierOutput -join [Environment]::NewLine))
    }
    $earlierBarrierMessages = @(
        $earlierBarrierOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $earlierWaitTerminal =
        Get-ProtocolMessage `
            -Messages $earlierBarrierMessages `
            -RequestId 'request-2' `
            -Status 'completed'
    $laterOpenTerminal =
        Get-ProtocolMessage `
            -Messages $earlierBarrierMessages `
            -RequestId 'request-3' `
            -Status 'completed'
    $earlierBarrierState =
        Get-ProtocolMessage `
            -Messages $earlierBarrierMessages `
            -RequestId 'request-4' `
            -Status 'completed'
    Assert-True `
        -Condition (
            [array]::IndexOf(
                $earlierBarrierMessages,
                $earlierWaitTerminal) -lt
                [array]::IndexOf(
                    $earlierBarrierMessages,
                    $laterOpenTerminal) -and
            [string]$earlierBarrierState.state.source.path -eq
                $sourceRootB) `
        -Message 'wait.idle must complete after its earlier sequence retires without waiting for a later accepted source.open.'
    $earlierBarrierPid =
        Get-LauncherProcessId -Output $earlierBarrierOutput
    Assert-True `
        -Condition (
            $null -eq (
                Get-Process `
                    -Id $earlierBarrierPid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'Earlier-only wait barrier should leave no GUI process.'

    $captureStateRoot =
        Join-Path $fixtureParent 'capture-state'
    $captureStatePath =
        Join-Path $captureStateRoot 'captures\projection.png'
    $captureBusyPath =
        Join-Path $captureStateRoot 'captures\busy.png'
    $captureStateOutput = @(
        @(
            'pipeline begin',
            "frame capture $captureStatePath",
            "frame capture $captureBusyPath",
            'state get',
            'pipeline end',
            'state get',
            'app quit'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $captureStateRoot 2>&1
    )
    $captureStateExitCode = $LASTEXITCODE
    if ($captureStateExitCode -ne 0) {
        throw (
            "Capture state workflow exited with $captureStateExitCode.`n" +
            ($captureStateOutput -join [Environment]::NewLine))
    }
    $captureStateMessages = @(
        $captureStateOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $capturePendingState =
        Get-ProtocolMessage `
            -Messages $captureStateMessages `
            -RequestId 'request-3' `
            -Status 'completed'
    $captureSucceededState =
        Get-ProtocolMessage `
            -Messages $captureStateMessages `
            -RequestId 'request-4' `
            -Status 'completed'
    $captureBusyTerminal =
        Get-ProtocolMessage `
            -Messages $captureStateMessages `
            -RequestId 'request-2' `
            -Status 'failed'
    Assert-True `
        -Condition (
            [string]$captureBusyTerminal.error.code -eq
                'capture_busy' -and
            [bool]$capturePendingState.state.capture.pending -and
            [string]$capturePendingState.state.capture.current_path -eq
                $captureStatePath -and
            [string]$capturePendingState.state.capture.last_result -eq
                'failed' -and
            [string]$capturePendingState.state.capture.last_path -eq
                $captureBusyPath -and
            -not [bool]$captureSucceededState.state.capture.pending -and
            [string]$captureSucceededState.state.capture.last_result -eq
                'succeeded' -and
            [string]$captureSucceededState.state.capture.last_path -eq
                $captureStatePath -and
            (Test-Path -LiteralPath $captureStatePath -PathType Leaf) -and
            (Get-Item -LiteralPath $captureStatePath).Length -gt 8 -and
            -not (Test-Path -LiteralPath $captureBusyPath)) `
        -Message 'capture_busy should update the last failed attempt while preserving the active pending path, followed by the first capture success.'

    $captureFailureRoot =
        Join-Path $fixtureParent 'capture-failure-state'
    $captureFailurePath =
        Join-Path $fixtureParent 'capture-failure-outside.png'
    $captureFailureOutput = @(
        @(
            'pipeline begin',
            "frame capture $captureFailurePath",
            'state get',
            'pipeline end',
            'app quit'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $captureFailureRoot 2>&1
    )
    $captureFailureExitCode = $LASTEXITCODE
    if ($captureFailureExitCode -ne 0) {
        throw (
            "Capture failure projection exited with $captureFailureExitCode.`n" +
            ($captureFailureOutput -join [Environment]::NewLine))
    }
    $captureFailureMessages = @(
        $captureFailureOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $captureRejected =
        Get-ProtocolMessage `
            -Messages $captureFailureMessages `
            -RequestId 'request-1' `
            -Status 'failed'
    $captureFailedState =
        Get-ProtocolMessage `
            -Messages $captureFailureMessages `
            -RequestId 'request-2' `
            -Status 'completed'
    Assert-True `
        -Condition (
            [string]$captureRejected.error.code -eq
                'capture_path_outside_state_root' -and
            -not [bool]$captureFailedState.state.capture.pending -and
            [string]$captureFailedState.state.capture.last_result -eq
                'failed' -and
            [string]$captureFailedState.state.capture.last_path -eq
                $captureFailurePath -and
            -not (Test-Path -LiteralPath $captureFailurePath)) `
        -Message 'state.capture should distinguish a stable failed terminal attempt without creating the rejected file.'

    $disconnectCaptureRoot =
        Join-Path $fixtureParent 'capture-disconnect-state'
    $disconnectCapturePath =
        Join-Path $disconnectCaptureRoot 'captures\disconnected.png'
    $disconnectCaptureOutput = @(
        @(
            'disconnect after accepted',
            "frame capture $disconnectCapturePath"
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $disconnectCaptureRoot 2>&1
    )
    $disconnectCaptureExitCode = $LASTEXITCODE
    if ($disconnectCaptureExitCode -ne 0) {
        throw (
            "Accepted capture disconnect exited with $disconnectCaptureExitCode.`n" +
            ($disconnectCaptureOutput -join [Environment]::NewLine))
    }
    $disconnectCaptureMessages = @(
        $disconnectCaptureOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $disconnectCaptureAccepted =
        Get-ProtocolMessage `
            -Messages $disconnectCaptureMessages `
            -RequestId 'request-1' `
            -Status 'accepted'
    $disconnectCapturePid =
        Get-LauncherProcessId -Output $disconnectCaptureOutput
    $disconnectTemporaryArtifacts = @(
        Get-ChildItem `
            -LiteralPath (
                Split-Path `
                    -Parent `
                    $disconnectCapturePath) `
            -Filter '.specforge-capture-*.tmp' `
            -Force `
            -ErrorAction SilentlyContinue
    )
    Assert-True `
        -Condition (
            [string]$disconnectCaptureAccepted.command -eq
                'frame.capture' -and
            -not (Test-Path -LiteralPath $disconnectCapturePath) -and
            -not (Test-Path -LiteralPath ($disconnectCapturePath + '.tmp')) -and
            $disconnectTemporaryArtifacts.Count -eq 0 -and
            $null -eq (
                Get-Process `
                    -Id $disconnectCapturePid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'A capture disconnected after acceptance must not encode a PNG or resurrect after launcher cleanup.'

    $canceledBusinessRoot =
        Join-Path $fixtureParent 'canceled-business-state'
    $canceledBusinessOutput = @(
        @(
            "source open $sourceRoot",
            'wait idle',
            'pipeline begin',
            'spectrum goto 2',
            "source open $sourceRootB",
            'wait idle',
            'pipeline end',
            "source open $sourceRoot",
            'wait idle',
            'pipeline begin',
            'label assign 5 spectrum 2',
            "source open $sourceRootB",
            'wait idle',
            'pipeline end',
            'app quit'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $canceledBusinessRoot `
                --labeling-state-seed $labelSeed 2>&1
    )
    $canceledBusinessExitCode = $LASTEXITCODE
    if ($canceledBusinessExitCode -ne 0) {
        throw (
            "Canceled business workflow exited with $canceledBusinessExitCode.`n" +
            ($canceledBusinessOutput -join [Environment]::NewLine))
    }
    $canceledBusinessMessages = @(
        $canceledBusinessOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $gotoCanceled =
        Get-ProtocolMessage `
            -Messages $canceledBusinessMessages `
            -RequestId 'request-3' `
            -Status 'failed'
    $labelCanceled =
        Get-ProtocolMessage `
            -Messages $canceledBusinessMessages `
            -RequestId 'request-8' `
            -Status 'failed'
    $canceledPersisted =
        Get-Content -Raw -LiteralPath (
            Join-Path $canceledBusinessRoot 'sample-labeling-tasks.json') |
            ConvertFrom-Json
    $canceledQualityTask = @(
        $canceledPersisted.sources |
            ForEach-Object { $_.tasks } |
            Where-Object {
                [string]$_.task_id -eq 'quality'
            }
    )[0]
    Assert-True `
        -Condition (
            [string]$gotoCanceled.error.code -eq
                'operation_canceled' -and
            [string]$labelCanceled.error.code -eq
                'operation_canceled' -and
            [int]$canceledQualityTask.values[2] -eq
                -1) `
        -Message 'Changed goto and targeted label should inherit operation_canceled when a newer source activation wins, without writing the label.'

    $sameActivationRoot =
        Join-Path $fixtureParent 'same-activation-state'
    $sameActivationOutput = @(
        @(
            "source open $sourceRoot",
            'wait idle',
            'spectrum goto 2',
            'wait idle',
            'pipeline begin',
            'spectrum goto 2',
            "source open $sourceRoot",
            'wait idle',
            'pipeline end',
            'pipeline begin',
            'label assign 5 spectrum 2',
            "source open $sourceRoot",
            'wait idle',
            'pipeline end',
            'app quit'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $sameActivationRoot `
                --labeling-state-seed $labelSeed 2>&1
    )
    $sameActivationExitCode = $LASTEXITCODE
    if ($sameActivationExitCode -ne 0) {
        throw (
            "Same-activation workflow exited with $sameActivationExitCode.`n" +
            ($sameActivationOutput -join [Environment]::NewLine))
    }
    $sameActivationMessages = @(
        $sameActivationOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $sameGotoTerminal =
        Get-ProtocolTerminalMessage `
            -Messages $sameActivationMessages `
            -RequestId 'request-5'
    $sameGotoLatestOpen =
        Get-ProtocolMessage `
            -Messages $sameActivationMessages `
            -RequestId 'request-6' `
            -Status 'completed'
    $sameLabelTerminal =
        Get-ProtocolTerminalMessage `
            -Messages $sameActivationMessages `
            -RequestId 'request-8'
    $sameLabelLatestOpen =
        Get-ProtocolMessage `
            -Messages $sameActivationMessages `
            -RequestId 'request-9' `
            -Status 'completed'
    $sameActivationPersisted =
        Get-Content -Raw -LiteralPath (
            Join-Path $sameActivationRoot 'sample-labeling-tasks.json') |
            ConvertFrom-Json
    $sameActivationQualityTask = @(
        $sameActivationPersisted.sources |
            ForEach-Object { $_.tasks } |
            Where-Object {
                [string]$_.task_id -eq 'quality'
            }
    )[0]
    $sameGotoTerminalIndex =
        [array]::IndexOf(
            $sameActivationMessages,
            $sameGotoTerminal)
    $sameGotoLatestOpenIndex =
        [array]::IndexOf(
            $sameActivationMessages,
            $sameGotoLatestOpen)
    $sameLabelTerminalIndex =
        [array]::IndexOf(
            $sameActivationMessages,
            $sameLabelTerminal)
    $sameLabelLatestOpenIndex =
        [array]::IndexOf(
            $sameActivationMessages,
            $sameLabelLatestOpen)
    $sameGotoCanceled =
        [string]$sameGotoTerminal.status -eq 'failed' -and
        [string]$sameGotoTerminal.error.code -eq
            'operation_canceled'
    $sameGotoCompletedFirst =
        [string]$sameGotoTerminal.status -eq 'completed' -and
        $sameGotoTerminalIndex -lt
            $sameGotoLatestOpenIndex -and
        [int]$sameGotoTerminal.result.spectrum.index -eq
            2
    $sameLabelCanceled =
        [string]$sameLabelTerminal.status -eq 'failed' -and
        [string]$sameLabelTerminal.error.code -eq
            'operation_canceled' -and
        [int]$sameActivationQualityTask.values[2] -eq
            -1
    $sameLabelCompletedFirst =
        [string]$sameLabelTerminal.status -eq 'completed' -and
        $sameLabelTerminalIndex -lt
            $sameLabelLatestOpenIndex -and
        [int]$sameLabelTerminal.result.assignment.spectrum.index -eq
            2 -and
        [int]$sameLabelTerminal.result.assignment.new_code -eq
            5 -and
        [int]$sameActivationQualityTask.values[2] -eq
            5
    Assert-True `
        -Condition (
            ($sameGotoCanceled -or
             $sameGotoCompletedFirst) -and
            [string]$sameGotoLatestOpen.result.source.path -eq
                $sourceRoot -and
            [int]$sameGotoLatestOpen.result.current_spectrum.index -eq
                2 -and
            ($sameLabelCanceled -or
             $sameLabelCompletedFirst) -and
            [string]$sameLabelLatestOpen.result.source.path -eq
                $sourceRoot -and
            [int]$sameLabelLatestOpen.result.current_spectrum.index -eq
                2) `
        -Message (
            'A newer same-path activation landing on the same row must either cancel an operation it supersedes or follow an already completed factual terminal; a canceled label cannot write and a written label must complete before replacement. Output: ' +
            ($sameActivationOutput -join [Environment]::NewLine))
    $sameActivationPid =
        Get-LauncherProcessId -Output $sameActivationOutput
    Assert-True `
        -Condition (
            $null -eq (
                Get-Process `
                    -Id $sameActivationPid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'Same-activation generation workflow should leave no GUI process.'

    $labelThenQuitRoot =
        Join-Path $fixtureParent 'label-then-quit-state'
    $labelThenQuitOutput = @(
        @(
            "source open $sourceRoot",
            'wait idle',
            'pipeline begin',
            'label assign 5',
            'app quit',
            'pipeline end'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $labelThenQuitRoot `
                --labeling-state-seed $labelSeed 2>&1
    )
    $labelThenQuitExitCode = $LASTEXITCODE
    if ($labelThenQuitExitCode -ne 0) {
        throw (
            "Label-then-quit workflow exited with $labelThenQuitExitCode.`n" +
            ($labelThenQuitOutput -join [Environment]::NewLine))
    }
    $labelThenQuitMessages = @(
        $labelThenQuitOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $claimedLabelTerminal =
        Get-ProtocolMessage `
            -Messages $labelThenQuitMessages `
            -RequestId 'request-3' `
            -Status 'completed'
    $waitingQuitTerminal =
        Get-ProtocolMessage `
            -Messages $labelThenQuitMessages `
            -RequestId 'request-4' `
            -Status 'completed'
    $claimedLabelIndex =
        [array]::IndexOf(
            $labelThenQuitMessages,
            $claimedLabelTerminal)
    $waitingQuitIndex =
        [array]::IndexOf(
            $labelThenQuitMessages,
            $waitingQuitTerminal)
    $labelThenQuitPersisted =
        Get-Content -Raw -LiteralPath (
            Join-Path $labelThenQuitRoot 'sample-labeling-tasks.json') |
            ConvertFrom-Json
    Assert-True `
        -Condition (
            $claimedLabelIndex -ge 0 -and
            $claimedLabelIndex -lt $waitingQuitIndex -and
            [int]$claimedLabelTerminal.result.assignment.new_code -eq
                5 -and
            [int]$labelThenQuitPersisted.sources[0].tasks[0].values[0] -eq
                5) `
        -Message 'A label write that occurs must report completed before the later app.quit terminal and persist the same fact.'

    $labelThenReplaceRoot =
        Join-Path $fixtureParent 'label-then-replace-state'
    $labelThenReplaceOutput = @(
        @(
            "source open $sourceRoot",
            'wait idle',
            'pipeline begin',
            'label assign 5',
            "source open $sourceRootB",
            'app quit',
            'pipeline end'
        ) |
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $labelThenReplaceRoot `
                --labeling-state-seed $labelSeed 2>&1
    )
    $labelThenReplaceExitCode = $LASTEXITCODE
    if ($labelThenReplaceExitCode -ne 0) {
        throw (
            "Label-then-replace workflow exited with $labelThenReplaceExitCode.`n" +
            ($labelThenReplaceOutput -join [Environment]::NewLine))
    }
    $labelThenReplaceMessages = @(
        $labelThenReplaceOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $labelThenReplaceInitialOpen =
        Get-ProtocolMessage `
            -Messages $labelThenReplaceMessages `
            -RequestId 'request-1' `
            -Status 'completed'
    $writtenLabelTerminal =
        Get-ProtocolMessage `
            -Messages $labelThenReplaceMessages `
            -RequestId 'request-3' `
            -Status 'completed'
    $replacementSourceTerminal =
        Get-ProtocolMessage `
            -Messages $labelThenReplaceMessages `
            -RequestId 'request-4' `
            -Status 'completed'
    $replacementQuitTerminal =
        Get-ProtocolMessage `
            -Messages $labelThenReplaceMessages `
            -RequestId 'request-5' `
            -Status 'completed'
    $writtenLabelIndex =
        [array]::IndexOf(
            $labelThenReplaceMessages,
            $writtenLabelTerminal)
    $replacementQuitIndex =
        [array]::IndexOf(
            $labelThenReplaceMessages,
            $replacementQuitTerminal)
    $labelThenReplacePersisted =
        Get-Content -Raw -LiteralPath (
            Join-Path $labelThenReplaceRoot 'sample-labeling-tasks.json') |
            ConvertFrom-Json
    $labelThenReplaceQualityTask = @(
        $labelThenReplacePersisted.sources |
            ForEach-Object { $_.tasks } |
            Where-Object {
                [string]$_.task_id -eq 'quality'
            }
    )[0]
    Assert-True `
        -Condition (
            $writtenLabelIndex -ge 0 -and
            $writtenLabelIndex -lt $replacementQuitIndex -and
            [string]$writtenLabelTerminal.result.assignment.source_id -eq
                [string]$labelThenReplaceInitialOpen.result.source.id -and
            [int]$writtenLabelTerminal.result.assignment.spectrum.index -eq
                0 -and
            [int]$writtenLabelTerminal.result.assignment.previous_code -eq
                -1 -and
            [int]$writtenLabelTerminal.result.assignment.new_code -eq
                5 -and
            [string]$replacementSourceTerminal.result.source.path -eq
                $sourceRootB -and
            [int]$labelThenReplaceQualityTask.values[0] -eq
                5) `
        -Message (
            'A label already written must report its factual completed terminal before app.quit even when a later source activation replaces its pending Present. Output: ' +
            ($labelThenReplaceOutput -join [Environment]::NewLine))
    $labelThenReplacePid =
        Get-LauncherProcessId -Output $labelThenReplaceOutput
    Assert-True `
        -Condition (
            $null -eq (
                Get-Process `
                    -Id $labelThenReplacePid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'Label-then-replace workflow should leave no GUI process.'

    $malformedRoot =
        Join-Path $fixtureParent 'malformed-source'
    [System.IO.Directory]::CreateDirectory(
        $malformedRoot) | Out-Null
    [System.IO.File]::WriteAllText(
        (Join-Path $malformedRoot 'alpha.csv'),
        "wavelength,flux`n5000,1`n5001,2`n",
        [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllBytes(
        (Join-Path $malformedRoot 'bravo.csv'),
        [byte[]]@())

    $gotoLoadFailure =
        Invoke-RejectedGuiWorkflow `
            -Commands @(
                "source open $malformedRoot",
                'wait idle',
                'spectrum goto 1'
            ) `
            -LauncherPath $resolvedLauncher `
            -AppPath $fixtureExecutable `
            -Root (
                Join-Path $fixtureParent 'goto-load-failure-state') `
            -Seed $labelSeed
    $labelLoadFailure =
        Invoke-RejectedGuiWorkflow `
            -Commands @(
                "source open $malformedRoot",
                'wait idle',
                'label assign 5 spectrum 1'
            ) `
            -LauncherPath $resolvedLauncher `
            -AppPath $fixtureExecutable `
            -Root (
                Join-Path $fixtureParent 'label-load-failure-state') `
            -Seed $labelSeed
    Assert-True `
        -Condition (
            [string]$gotoLoadFailure.Messages[6].error.code -eq
                'spectrum_load_failed' -and
            [string]$labelLoadFailure.Messages[6].error.code -eq
                'spectrum_load_failed' -and
            $null -eq (
                Get-Process `
                    -Id $gotoLoadFailure.Pid `
                    -ErrorAction SilentlyContinue) -and
            $null -eq (
                Get-Process `
                    -Id $labelLoadFailure.Pid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'Changed goto and targeted label should both publish the documented spectrum_load_failed terminal code.'

    $missingSource =
        Join-Path $fixtureParent 'missing-source'
    $missingSourceFailure =
        Invoke-RejectedGuiWorkflow `
            -Commands @(
                "source open $missingSource"
            ) `
            -LauncherPath $resolvedLauncher `
            -AppPath $fixtureExecutable `
            -Root (
                Join-Path $fixtureParent 'missing-source-state') `
            -Seed $labelSeed
    Assert-True `
        -Condition (
            $missingSourceFailure.ExitCode -ne 0 -and
            [string]$missingSourceFailure.Messages[2].status -eq
                'failed' -and
            [string]$missingSourceFailure.Messages[2].error.code -eq
                'source_not_found' -and
            $null -eq (
                Get-Process `
                    -Id $missingSourceFailure.Pid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'source.open should reject a missing absolute source and clean up its owned visible GUI.'

    $missingSpectrumFailure =
        Invoke-RejectedGuiWorkflow `
            -Commands @(
                "source open $sourceRoot",
                'wait idle',
                'spectrum goto name missing.csv'
            ) `
            -LauncherPath $resolvedLauncher `
            -AppPath $fixtureExecutable `
            -Root (
                Join-Path $fixtureParent 'missing-spectrum-state') `
            -Seed $labelSeed
    Assert-True `
        -Condition (
            $missingSpectrumFailure.ExitCode -ne 0 -and
            [string]$missingSpectrumFailure.Messages[6].status -eq
                'failed' -and
            [string]$missingSpectrumFailure.Messages[6].error.code -eq
                'spectrum_not_found' -and
            $null -eq (
                Get-Process `
                    -Id $missingSpectrumFailure.Pid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'spectrum.goto should distinguish a missing exact spectrum and clean up its owned GUI.'

    $invalidLabelFailure =
        Invoke-RejectedGuiWorkflow `
            -Commands @(
                "source open $sourceRoot",
                'wait idle',
                'label assign 999'
            ) `
            -LauncherPath $resolvedLauncher `
            -AppPath $fixtureExecutable `
            -Root (
                Join-Path $fixtureParent 'invalid-label-state') `
            -Seed $labelSeed
    Assert-True `
        -Condition (
            $invalidLabelFailure.ExitCode -ne 0 -and
            [string]$invalidLabelFailure.Messages[6].status -eq
                'failed' -and
            [string]$invalidLabelFailure.Messages[6].error.code -eq
                'label_not_found' -and
            $null -eq (
                Get-Process `
                    -Id $invalidLabelFailure.Pid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'label.assign should reject a code absent from the active production task and clean up its owned GUI.'

    $invalidSeed =
        Join-Path $fixtureParent 'invalid-labeling-seed.json'
    [System.IO.File]::WriteAllText(
        $invalidSeed,
        '{ invalid json',
        [System.Text.UTF8Encoding]::new($false))
    $invalidSeedRoot =
        Join-Path $fixtureParent 'invalid-seed-state'
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $invalidSeedOutput = @(
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $invalidSeedRoot `
                --labeling-state-seed $invalidSeed 2>&1
        )
        $invalidSeedExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    Assert-True `
        -Condition (
            $invalidSeedExitCode -ne 0 -and
            -not (Test-Path -LiteralPath $invalidSeedRoot) -and
            -not (@($invalidSeedOutput) -match '^SpecForge PID:')) `
        -Message 'Invalid production seed data must fail before creating the GUI process or retaining a partial root.'

    $ordinarySeedRoot =
        Join-Path $fixtureParent 'ordinary-seed-state'
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $ordinarySeedOutput = @(
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $ordinarySeedRoot `
                --labeling-state-seed (
                    Join-Path $ordinaryRoot 'ui-language.json') 2>&1
        )
        $ordinarySeedExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    Assert-True `
        -Condition (
            $ordinarySeedExitCode -ne 0 -and
            -not (Test-Path -LiteralPath $ordinarySeedRoot) -and
            -not (@($ordinarySeedOutput) -match '^SpecForge PID:')) `
        -Message 'Launcher must reject a seed inside the ordinary user state root before GUI startup.'

    $ordinaryOutputSeed =
        Join-Path $fixtureParent 'ordinary-output-seed.json'
    $ordinaryOutputPath =
        Join-Path $ordinaryRoot 'seed-output.csv'
    & $resolvedStateFixture `
        --write-labeling-seed $ordinaryOutputSeed `
        --source $sourceRoot `
        --output-path $ordinaryOutputPath
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not generate the production ordinary-output seed fixture.'
    }
    $ordinaryOutputSeedBefore =
        Get-FileSha256 -Path $ordinaryOutputSeed
    $ordinaryOutputSeedRoot =
        Join-Path $fixtureParent 'ordinary-output-seed-state'
    $ordinaryBeforeOutputSeed =
        Get-TreeFingerprint -Path $ordinaryRoot
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $ordinaryOutputSeedOutput = @(
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $ordinaryOutputSeedRoot `
                --labeling-state-seed $ordinaryOutputSeed 2>&1
        )
        $ordinaryOutputSeedExitCode =
            $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    Assert-True `
        -Condition (
            $ordinaryOutputSeedExitCode -ne 0 -and
            -not (Test-Path -LiteralPath $ordinaryOutputSeedRoot) -and
            -not (@($ordinaryOutputSeedOutput) -match '^SpecForge PID:') -and
            -not (Test-Path -LiteralPath $ordinaryOutputPath) -and
            (Get-TreeFingerprint -Path $ordinaryRoot) -ceq
                $ordinaryBeforeOutputSeed -and
            (Get-FileSha256 -Path $ordinaryOutputSeed) -ceq
                $ordinaryOutputSeedBefore) `
        -Message 'A production seed output_path overlapping ordinary state must fail before GUI startup without changing either state or seed.'

    $internalOutputSeed =
        Join-Path $fixtureParent 'internal-output-seed.json'
    $internalOutputSeedRoot =
        Join-Path $fixtureParent 'internal-output-seed-state'
    $internalOutputPath =
        Join-Path $internalOutputSeedRoot 'labels\quality.npy'
    & $resolvedStateFixture `
        --write-labeling-seed $internalOutputSeed `
        --source $sourceRoot `
        --output-path $internalOutputPath
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not generate the production root-contained output seed fixture.'
    }
    $internalOutputSeedBefore =
        Get-FileSha256 -Path $internalOutputSeed
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $internalOutputSeedOutput = @(
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $internalOutputSeedRoot `
                --labeling-state-seed $internalOutputSeed 2>&1
        )
        $internalOutputSeedExitCode =
            $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    Assert-True `
        -Condition (
            $internalOutputSeedExitCode -ne 0 -and
            -not (Test-Path -LiteralPath $internalOutputSeedRoot) -and
            -not (Test-Path -LiteralPath $internalOutputPath) -and
            -not (@($internalOutputSeedOutput) -match '^SpecForge PID:') -and
            (@($internalOutputSeedOutput) -match
                'Persistent labeling output paths are not permitted').Count -gt
                    0 -and
            (Get-FileSha256 -Path $internalOutputSeed) -ceq
                $internalOutputSeedBefore) `
        -Message 'Automation seeding must reject every persistent output_path, including a path lexically inside the future isolated root, before GUI startup.'

    $externalOutputSeed =
        Join-Path $fixtureParent 'external-output-seed.json'
    $externalOutputPath =
        Join-Path $externalSentinelRoot 'seed-output.csv'
    & $resolvedStateFixture `
        --write-labeling-seed $externalOutputSeed `
        --source $sourceRoot `
        --output-path $externalOutputPath
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not generate the production external-output seed fixture.'
    }
    $externalOutputSeedBefore =
        Get-FileSha256 -Path $externalOutputSeed
    $externalOutputSeedRoot =
        Join-Path $fixtureParent 'external-output-seed-state'
    $externalBeforeOutputSeed =
        Get-TreeFingerprint -Path $externalSentinelRoot
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $externalOutputSeedOutput = @(
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $externalOutputSeedRoot `
                --labeling-state-seed $externalOutputSeed 2>&1
        )
        $externalOutputSeedExitCode =
            $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    Assert-True `
        -Condition (
            $externalOutputSeedExitCode -ne 0 -and
            -not (Test-Path -LiteralPath $externalOutputSeedRoot) -and
            -not (@($externalOutputSeedOutput) -match '^SpecForge PID:') -and
            -not (Test-Path -LiteralPath $externalOutputPath) -and
            (Get-TreeFingerprint -Path $externalSentinelRoot) -ceq
                $externalBeforeOutputSeed -and
            (Get-FileSha256 -Path $externalOutputSeed) -ceq
                $externalOutputSeedBefore) `
        -Message 'A production seed output_path outside the automation root must fail before GUI startup without external writes.'

    $reparseTarget =
        Join-Path $fixtureParent 'reparse-target'
    $reparseParent =
        Join-Path $fixtureParent 'reparse-parent'
    [System.IO.Directory]::CreateDirectory(
        $reparseTarget) | Out-Null
    $reparseCommandOutput = @(
        & $env:ComSpec /d /c `
            "mklink /J `"$reparseParent`" `"$reparseTarget`"" 2>&1
    )
    if ($LASTEXITCODE -ne 0) {
        throw (
            'Could not create the launcher reparse regression fixture: ' +
            ($reparseCommandOutput -join [Environment]::NewLine))
    }
    $reparseSeedRoot =
        Join-Path $reparseParent 'state'
    $reparseOutputSeed =
        Join-Path $fixtureParent 'reparse-output-seed.json'
    & $resolvedStateFixture `
        --write-labeling-seed $reparseOutputSeed `
        --source $sourceRoot `
        --output-path (
            Join-Path $reparseSeedRoot 'labels\quality.csv')
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not generate the production reparse-output seed fixture.'
    }
    $reparseTargetBefore =
        Get-TreeFingerprint -Path $reparseTarget
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $reparseSeedOutput = @(
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $reparseSeedRoot `
                --labeling-state-seed $reparseOutputSeed 2>&1
        )
        $reparseSeedExitCode =
            $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    Assert-True `
        -Condition (
            $reparseSeedExitCode -ne 0 -and
            -not (Test-Path -LiteralPath (
                Join-Path $reparseTarget 'state')) -and
            -not (@($reparseSeedOutput) -match '^SpecForge PID:') -and
            (Get-TreeFingerprint -Path $reparseTarget) -ceq
                $reparseTargetBefore) `
        -Message 'A reparse-point state parent must reject seeded persistent output references before GUI startup or target writes.'

    $existingRoot =
        Join-Path $fixtureParent 'existing-state'
    [System.IO.Directory]::CreateDirectory(
        $existingRoot) | Out-Null
    $existingMarker =
        Join-Path $existingRoot 'marker.txt'
    [System.IO.File]::WriteAllText(
        $existingMarker,
        'preserve',
        [System.Text.UTF8Encoding]::new($false))
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $existingRootOutput = @(
            & $resolvedLauncher `
                --app $fixtureExecutable `
                --state-root $existingRoot `
                --labeling-state-seed $labelSeed 2>&1
        )
        $existingRootExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    Assert-True `
        -Condition (
            $existingRootExitCode -ne 0 -and
            (Get-Content -Raw -LiteralPath $existingMarker) -ceq 'preserve' -and
            -not (@($existingRootOutput) -match '^SpecForge PID:')) `
        -Message 'Launcher must reject every existing target root without modifying it or starting the GUI.'

    $outsideCapture =
        Join-Path $fixtureParent 'outside-capture.png'
    $captureRejectRoot =
        Join-Path $fixtureParent 'capture-reject-state'
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $captureRejectOutput = @(
            "frame capture $outsideCapture" |
                & $resolvedLauncher `
                    --app $fixtureExecutable `
                    --state-root $captureRejectRoot `
                    --labeling-state-seed $labelSeed 2>&1
        )
        $captureRejectExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    $captureRejectMessages = @(
        $captureRejectOutput |
            Where-Object { [string]$_ -match '^\{' } |
            ForEach-Object {
                [string]$_ | ConvertFrom-Json
            }
    )
    $captureRejectPid =
        Get-LauncherProcessId `
            -Output $captureRejectOutput
    Assert-True `
        -Condition (
            $captureRejectExitCode -ne 0 -and
            -not (Test-Path -LiteralPath $outsideCapture) -and
            [string]$captureRejectMessages[2].status -eq 'failed' -and
            [string]$captureRejectMessages[2].error.code -eq
                'capture_path_outside_state_root' -and
            $null -eq (
                Get-Process `
                    -Id $captureRejectPid `
                    -ErrorAction SilentlyContinue)) `
        -Message 'frame.capture must reject an absolute path outside the current automation root and clean up its owned GUI.'

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

    $pipelineLimitRoot =
        Join-Path $fixtureParent 'pipeline-limit-state'
    [System.Environment]::SetEnvironmentVariable(
        $cleanupFixtureEnvironment,
        'pipeline-limit',
        [System.EnvironmentVariableTarget]::Process)
    $pipelineLimitInput = @(
        'pipeline begin'
        foreach ($index in 1..33) {
            'state get'
        }
        'pipeline end'
    )
    $savedErrorActionPreference =
        $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $pipelineLimitOutput = @(
            $pipelineLimitInput |
                & $resolvedLauncher `
                    --app $resolvedCleanupFixture `
                    --state-root $pipelineLimitRoot 2>&1
        )
        $pipelineLimitExitCode =
            $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference =
            $savedErrorActionPreference
    }
    $pipelineLimitPid =
        Get-LauncherProcessId `
            -Output $pipelineLimitOutput
    Assert-True `
        -Condition (
            $pipelineLimitExitCode -ne 0 -and
            [string]($pipelineLimitOutput -join "`n") -match
                'Pipeline accepts at most 32 commands' -and
            $null -eq (
                Get-Process `
                    -Id $pipelineLimitPid `
                    -ErrorAction SilentlyContinue) -and
            -not (Test-Path `
                -LiteralPath (
                    Join-Path `
                        $pipelineLimitRoot `
                        'pipeline-command-observed.txt')) -and
            (Test-Path `
                -LiteralPath (
                    Join-Path `
                        $pipelineLimitRoot `
                        'launcher-cleanup-completed.txt') `
                -PathType Leaf)) `
        -Message (
            'An oversized pipeline must fail before sending business requests and still clean up its owned child. Output: ' +
            ($pipelineLimitOutput -join [Environment]::NewLine))

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
        $removed = $false
        for ($attempt = 0;
             $attempt -lt 20 -and -not $removed;
             ++$attempt) {
            try {
                Remove-Item `
                    -LiteralPath $resolvedFixture `
                    -Recurse `
                    -Force `
                    -ErrorAction Stop
                $removed = $true
            }
            catch {
                if ($attempt -eq 19) {
                    throw
                }
                Start-Sleep -Milliseconds 50
            }
        }
    }
}

Write-Host 'automation launcher integration tests passed'
