<#
.SYNOPSIS
Runs a bounded, deterministic production ASDF mutation corpus.

.EXAMPLE
scripts\run-asdf-labeling-mutation-corpus.ps1 `
  -Cases 100000 `
  -Seed 20260831 `
  -ArtifactsDirectory build\asdf-labeling-mutation-long-run
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Seed = '0x78c0de2026',
    [Alias('Cases')]
    [ValidateRange(1, 1000000)]
    [int]$CaseCount = 73,
    [ValidateRange(-1, [long]::MaxValue)]
    [long]$CaseIndex = -1,
    [string]$BaseFixture = '',
    [string]$ReplayFile = '',
    [ValidateRange(1, 8388608)]
    [int]$MaxInputBytes = 1048576,
    [Alias('ArtifactsDirectory')]
    [string]$OutputDirectory = '',
    [ValidateRange(-1, [long]::MaxValue)]
    [long]$InjectFailureCase = -1,
    [ValidateRange(-1, [long]::MaxValue)]
    [long]$InjectAbruptExitCase = -1,
    [ValidateRange(60, 3600)]
    [int]$BuildTimeoutSec = 900
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $PSScriptRoot 'build-ninja-msvc-debug.ps1'
$preset = 'ninja-msvc-debug'
$targetName = 'specforge_sample_labeling_asdf_mutation_tests'
$executablePath = Join-Path $repoRoot (
    'build\' + $preset + '\' + $targetName + '.exe')

if ($ReplayFile -and $CaseIndex -lt 0) {
    throw 'ReplayFile requires CaseIndex from reproducer.json.'
}

if (-not $OutputDirectory) {
    $runStamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' +
        [Guid]::NewGuid().ToString('N').Substring(0, 8)
    $OutputDirectory = Join-Path $repoRoot (
        'build\asdf-labeling-mutation-corpus\' + $runStamp)
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot $OutputDirectory
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) {
    if (-not (Test-Path -LiteralPath $OutputDirectory -PathType Container)) {
        throw "Mutation corpus output path is not a directory: $OutputDirectory"
    }
    if (@(Get-ChildItem -LiteralPath $OutputDirectory -Force).Count -gt 0) {
        throw "Mutation corpus output directory must be empty: $OutputDirectory"
    }
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

function ConvertTo-NativeCommandLineArgument {
    param([Parameter(Mandatory = $true)] [AllowEmptyString()] [string]$Value)

    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') {
        return $Value
    }
    $quoted = New-Object System.Text.StringBuilder
    [void]$quoted.Append('"')
    $backslashCount = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') {
            $backslashCount += 1
            continue
        }
        if ($character -eq '"') {
            [void]$quoted.Append(('\' * ($backslashCount * 2 + 1)))
            [void]$quoted.Append('"')
            $backslashCount = 0
            continue
        }
        if ($backslashCount -gt 0) {
            [void]$quoted.Append(('\' * $backslashCount))
            $backslashCount = 0
        }
        [void]$quoted.Append($character)
    }
    if ($backslashCount -gt 0) {
        [void]$quoted.Append(('\' * ($backslashCount * 2)))
    }
    [void]$quoted.Append('"')
    return $quoted.ToString()
}

$stdoutPath = Join-Path $OutputDirectory 'stdout.log'
$stderrPath = Join-Path $OutputDirectory 'stderr.log'
$manifestPath = Join-Path $OutputDirectory 'run.json'
$startedAt = [DateTime]::UtcNow
$exitCode = $null

Push-Location $repoRoot
try {
    & $buildScript -Configure -Preset $preset -TimeoutSec $BuildTimeoutSec
    & $buildScript `
        -Preset $preset `
        -Target $targetName `
        -TimeoutSec $BuildTimeoutSec

    if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
        throw "Mutation corpus executable is missing: $executablePath"
    }

    $arguments = New-Object System.Collections.Generic.List[string]
    $arguments.Add('--seed')
    $arguments.Add($Seed)
    $arguments.Add('--case-count')
    $arguments.Add($CaseCount.ToString([Globalization.CultureInfo]::InvariantCulture))
    $arguments.Add('--max-input-bytes')
    $arguments.Add($MaxInputBytes.ToString([Globalization.CultureInfo]::InvariantCulture))
    $arguments.Add('--artifact-directory')
    $arguments.Add($OutputDirectory)
    if ($CaseIndex -ge 0) {
        $arguments.Add('--case-index')
        $arguments.Add($CaseIndex.ToString([Globalization.CultureInfo]::InvariantCulture))
    }
    if ($BaseFixture) {
        $resolvedBaseFixture = if (Test-Path -LiteralPath $BaseFixture -PathType Leaf) {
            [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $BaseFixture))
        }
        else {
            $BaseFixture
        }
        $arguments.Add('--base-fixture')
        $arguments.Add($resolvedBaseFixture)
    }
    if ($ReplayFile) {
        $resolvedReplay = if ([System.IO.Path]::IsPathRooted($ReplayFile)) {
            [System.IO.Path]::GetFullPath($ReplayFile)
        }
        else {
            [System.IO.Path]::GetFullPath((Join-Path $repoRoot $ReplayFile))
        }
        $arguments.Add('--replay-file')
        $arguments.Add($resolvedReplay)
    }
    if ($InjectFailureCase -ge 0) {
        $arguments.Add('--inject-failure-case')
        $arguments.Add($InjectFailureCase.ToString(
            [Globalization.CultureInfo]::InvariantCulture))
    }
    if ($InjectAbruptExitCase -ge 0) {
        $arguments.Add('--inject-abrupt-exit-case')
        $arguments.Add($InjectAbruptExitCase.ToString(
            [Globalization.CultureInfo]::InvariantCulture))
    }

    $nativeArguments = @($arguments | ForEach-Object {
        ConvertTo-NativeCommandLineArgument -Value $_
    })
    $commandLine = (ConvertTo-NativeCommandLineArgument -Value $executablePath) +
        ' ' + ($nativeArguments -join ' ')
    $commit = (& git rev-parse HEAD).Trim()
    $workingTree = @(& git status --porcelain=v1)
    $environmentNames = @(
        'OS',
        'PROCESSOR_ARCHITECTURE',
        'NUMBER_OF_PROCESSORS',
        'SystemRoot',
        'TEMP',
        'TMP'
    )
    $environment = [ordered]@{}
    foreach ($name in $environmentNames) {
        $value = [Environment]::GetEnvironmentVariable($name)
        if ($null -ne $value) {
            $environment[$name] = $value
        }
    }
    $manifest = [ordered]@{
        format_kind = 'specforge.asdf_mutation_corpus_run'
        schema_version = 1
        started_at_utc = $startedAt.ToString('o')
        repository = $repoRoot
        commit = $commit
        working_tree_status = $workingTree
        preset = $preset
        target = $targetName
        executable_sha256 = (Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash
        command_line = $commandLine
        parameters = [ordered]@{
            seed = $Seed
            case_count = $CaseCount
            case_index = if ($CaseIndex -ge 0) { $CaseIndex } else { $null }
            base_fixture = if ($BaseFixture) { $BaseFixture } else { $null }
            replay_file = if ($ReplayFile) { $ReplayFile } else { $null }
            max_input_bytes = $MaxInputBytes
            inject_failure_case = if ($InjectFailureCase -ge 0) {
                $InjectFailureCase
            }
            else {
                $null
            }
            inject_abrupt_exit_case = if ($InjectAbruptExitCase -ge 0) {
                $InjectAbruptExitCase
            }
            else {
                $null
            }
        }
        environment_allowlist = $environmentNames
        environment = $environment
        exit_code = $null
        completed_at_utc = $null
    }
    $manifest | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath $manifestPath -Encoding utf8

    $process = Start-Process `
        -FilePath $executablePath `
        -ArgumentList $nativeArguments `
        -WorkingDirectory $repoRoot `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath `
        -WindowStyle Hidden `
        -Wait `
        -PassThru
    $exitCode = $process.ExitCode
    $manifest.exit_code = $exitCode
    $manifest.completed_at_utc = [DateTime]::UtcNow.ToString('o')
    $manifest | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath $manifestPath -Encoding utf8

    if (Test-Path -LiteralPath $stdoutPath) {
        Get-Content -LiteralPath $stdoutPath | Write-Host
    }
    if ($exitCode -ne 0) {
        if (Test-Path -LiteralPath $stderrPath) {
            Get-Content -LiteralPath $stderrPath | Write-Error
        }
        throw "Mutation corpus failed with exit code $exitCode. Artifacts: $OutputDirectory"
    }

    Write-Host "Mutation corpus passed. Evidence: $OutputDirectory"
}
finally {
    Pop-Location
}
