[CmdletBinding(PositionalBinding = $false)]
param(
    [ValidateRange(1000, 1000000)]
    [int]$SampleCount = 1000000,
    [ValidateRange(0, 10)]
    [int]$WarmupCount = 1,
    [ValidateRange(1, 20)]
    [int]$Iterations = 5,
    [string]$OutputDirectory = '',
    [ValidateRange(60, 7200)]
    [int]$BuildTimeoutSec = 1800
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $PSScriptRoot 'build-ninja-msvc-debug.ps1'
$Preset = 'ninja-msvc-release-static-asdf-labeling-hardening'
$targetName = 'spectiary_asdf_labeling_production_benchmark'
$executablePath = Join-Path $repoRoot (
    'build\' + $Preset + '\tools\' + $targetName + '.exe')

if (-not $OutputDirectory) {
    $runStamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' +
        [Guid]::NewGuid().ToString('N').Substring(0, 8)
    $OutputDirectory = Join-Path $repoRoot (
        'build\asdf-labeling-production-benchmark-results\' + $runStamp)
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot $OutputDirectory
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) {
    if (-not (Test-Path -LiteralPath $OutputDirectory -PathType Container)) {
        throw "Benchmark output path is not a directory: $OutputDirectory"
    }
    if (@(Get-ChildItem -LiteralPath $OutputDirectory -Force).Count -gt 0) {
        throw "Benchmark output directory must be empty: $OutputDirectory"
    }
}
$workRoot = Join-Path $OutputDirectory 'work'
$stdoutRoot = Join-Path $OutputDirectory 'stdout'
$stderrRoot = Join-Path $OutputDirectory 'stderr'
New-Item -ItemType Directory -Path $workRoot -Force | Out-Null
New-Item -ItemType Directory -Path $stdoutRoot -Force | Out-Null
New-Item -ItemType Directory -Path $stderrRoot -Force | Out-Null

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

function Get-Statistics {
    param([Parameter(Mandatory = $true)] [double[]]$Values)

    $orderedValues = @($Values | Sort-Object)
    if ($orderedValues.Count -eq 0) {
        throw 'Cannot summarize an empty benchmark series.'
    }
    $middle = [int][Math]::Floor($orderedValues.Count / 2)
    $median = if (($orderedValues.Count % 2) -eq 0) {
        ($orderedValues[$middle - 1] + $orderedValues[$middle]) / 2.0
    }
    else {
        $orderedValues[$middle]
    }
    return [ordered]@{
        min = $orderedValues[0]
        median = $median
        max = $orderedValues[$orderedValues.Count - 1]
    }
}

function Get-RecordedEnvironment {
    $variableAllowlist = @(
        'COMPUTERNAME',
        'OS',
        'PROCESSOR_ARCHITECTURE',
        'PROCESSOR_IDENTIFIER',
        'PROCESSOR_LEVEL',
        'PROCESSOR_REVISION',
        'NUMBER_OF_PROCESSORS',
        'SystemDrive',
        'SystemRoot',
        'TEMP',
        'TMP',
        'VCPKG_ROOT',
        'CMAKE_BUILD_PARALLEL_LEVEL'
    )
    $variables = [ordered]@{}
    foreach ($name in $variableAllowlist) {
        $value = [Environment]::GetEnvironmentVariable($name)
        if ($null -ne $value) {
            $variables[$name] = $value
        }
    }
    return [ordered]@{
        captured_at_utc = [DateTime]::UtcNow.ToString('o')
        current_directory = $repoRoot
        powershell_version = $PSVersionTable.PSVersion.ToString()
        process_architecture = [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()
        os_description = [Runtime.InteropServices.RuntimeInformation]::OSDescription
        recorded_variable_allowlist = $variableAllowlist
        process_environment = $variables
    }
}

Push-Location $repoRoot
try {
    $commit = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $commit) {
        throw 'Could not resolve the benchmark source commit.'
    }
    $workingTreeStatus = @(& git status --porcelain=v1)
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not inspect the benchmark working tree.'
    }

    $buildCommands = @(
        [ordered]@{
            command_line = '& scripts\build-ninja-msvc-debug.ps1 -Configure -Preset ' +
                $Preset + ' -TimeoutSec ' + $BuildTimeoutSec
        },
        [ordered]@{
            command_line = '& scripts\build-ninja-msvc-debug.ps1 -Preset ' +
                $Preset + ' -Target ' + $targetName + ' -TimeoutSec ' +
                $BuildTimeoutSec
        }
    )

    & $buildScript -Configure -Preset $Preset -TimeoutSec $BuildTimeoutSec
    & $buildScript `
        -Preset $Preset `
        -Target $targetName `
        -TimeoutSec $BuildTimeoutSec

    if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
        throw "Production benchmark executable is missing: $executablePath"
    }

    $environment = Get-RecordedEnvironment
    $cases = @('source-index', 'explicit-unicode')
    $operations = @(
        'initial-write',
        'read-open',
        'values-rewrite-1',
        'values-rewrite-1000',
        'metadata-rewrite'
    )
    $commands = New-Object System.Collections.Generic.List[object]
    $warmups = New-Object System.Collections.Generic.List[object]
    $measurements = New-Object System.Collections.Generic.List[object]
    $failures = New-Object System.Collections.Generic.List[string]
    $warmupJsonl = Join-Path $OutputDirectory 'warmup.jsonl'
    $measurementJsonl = Join-Path $OutputDirectory 'measurements.jsonl'

    function Invoke-BenchmarkProcess {
        param(
            [Parameter(Mandatory = $true)] [string]$Case,
            [Parameter(Mandatory = $true)] [string]$Operation,
            [Parameter(Mandatory = $true)] [string]$Phase,
            [Parameter(Mandatory = $true)] [int]$Iteration
        )

        $runId = $Case + '-' + $Operation + '-' + $Phase + '-' + $Iteration +
            '-' + [Guid]::NewGuid().ToString('N')
        $iterationDirectory = Join-Path $workRoot $runId
        $stdoutPath = Join-Path $stdoutRoot ($runId + '.json')
        $stderrPath = Join-Path $stderrRoot ($runId + '.log')
        New-Item -ItemType Directory -Path $iterationDirectory | Out-Null

        $arguments = @(
            '--case', $Case,
            '--operation', $Operation,
            '--sample-count', [string]$SampleCount,
            '--directory', $iterationDirectory,
            '--commit', $commit
        )
        $argumentLine = (($arguments | ForEach-Object {
                    ConvertTo-NativeCommandLineArgument -Value $_
                }) -join ' ')
        $commandLine = (ConvertTo-NativeCommandLineArgument -Value $executablePath) +
            ' ' + $argumentLine
        $startedAt = [DateTime]::UtcNow
        try {
            $benchmarkProcess = Start-Process `
                -FilePath $executablePath `
                -ArgumentList $argumentLine `
                -WorkingDirectory $repoRoot `
                -RedirectStandardOutput $stdoutPath `
                -RedirectStandardError $stderrPath `
                -WindowStyle Hidden `
                -Wait `
                -PassThru
            $finishedAt = [DateTime]::UtcNow
            $rawJson = (Get-Content -LiteralPath $stdoutPath -Raw).Trim()
            if (-not $rawJson) {
                throw "Benchmark process emitted no JSON; stderr: $stderrPath"
            }
            try {
                $result = $rawJson | ConvertFrom-Json
            }
            catch {
                throw "Benchmark process emitted invalid JSON: $stdoutPath"
            }
            $commands.Add([ordered]@{
                    case = $Case
                    operation = $Operation
                    phase = $Phase
                    iteration = $Iteration
                    started_at_utc = $startedAt.ToString('o')
                    finished_at_utc = $finishedAt.ToString('o')
                    executable = $executablePath
                    arguments = $arguments
                    command_line = $commandLine
                    working_directory = $repoRoot
                    data_directory = $iterationDirectory
                    exit_code = $benchmarkProcess.ExitCode
                    stdout_path = $stdoutPath
                    stderr_path = $stderrPath
                })

            if ($Phase -eq 'warmup') {
                Add-Content -LiteralPath $warmupJsonl -Value $rawJson -Encoding UTF8
                $warmups.Add($result)
            }
            else {
                Add-Content -LiteralPath $measurementJsonl -Value $rawJson -Encoding UTF8
                $measurements.Add($result)
            }
            if ($benchmarkProcess.ExitCode -ne 0 -or -not $result.success) {
                $failures.Add(
                    "$Case/$Operation/$Phase/$Iteration failed with " +
                    "exit $($benchmarkProcess.ExitCode), error_kind=$($result.error_kind)")
            }
        }
        finally {
            $resolvedWorkRoot = [System.IO.Path]::GetFullPath($workRoot)
            $resolvedIterationDirectory =
                [System.IO.Path]::GetFullPath($iterationDirectory)
            if ($resolvedIterationDirectory.StartsWith(
                    $resolvedWorkRoot + [System.IO.Path]::DirectorySeparatorChar,
                    [StringComparison]::OrdinalIgnoreCase) -and
                (Test-Path -LiteralPath $resolvedIterationDirectory)) {
                Remove-Item -LiteralPath $resolvedIterationDirectory -Recurse -Force
            }
        }
    }

    foreach ($benchmarkCase in $cases) {
        foreach ($benchmarkOperation in $operations) {
            for ($warmupIndex = 1; $warmupIndex -le $WarmupCount; $warmupIndex += 1) {
                Invoke-BenchmarkProcess `
                    -Case $benchmarkCase `
                    -Operation $benchmarkOperation `
                    -Phase 'warmup' `
                    -Iteration $warmupIndex
            }
            for ($iterationIndex = 1; $iterationIndex -le $Iterations; $iterationIndex += 1) {
                Invoke-BenchmarkProcess `
                    -Case $benchmarkCase `
                    -Operation $benchmarkOperation `
                    -Phase 'measurement' `
                    -Iteration $iterationIndex
            }
        }
    }

    $summaries = New-Object System.Collections.Generic.List[object]
    $numericMetrics = @(
        'wall_time_ms',
        'cpu_time_ms',
        'file_size_bytes',
        'working_set_before',
        'sampled_peak_working_set',
        'private_bytes_before',
        'sampled_peak_private_bytes',
        'process_peak_working_set'
    )
    foreach ($benchmarkCase in $cases) {
        foreach ($benchmarkOperation in $operations) {
            $series = @($measurements | Where-Object {
                    $_.case -eq $benchmarkCase -and
                    $_.operation -eq $benchmarkOperation
                })
            $summary = [ordered]@{
                case = $benchmarkCase
                operation = $benchmarkOperation
                sample_count = $SampleCount
                roster_kind = $series[0].roster_kind
                roster_width = $series[0].roster_width
                iterations = $series.Count
                roster_block_reused = @($series.roster_block_reused | Sort-Object -Unique)
                success = ($series.Count -eq $Iterations -and
                    @($series | Where-Object { -not $_.success }).Count -eq 0)
            }
            foreach ($metric in $numericMetrics) {
                $summary[$metric] = Get-Statistics -Values @(
                    $series | ForEach-Object { [double]$_.$metric })
            }
            $summaries.Add($summary)
        }
    }

    $bundle = [ordered]@{
        schema_version = 1
        generated_at_utc = [DateTime]::UtcNow.ToString('o')
        commit = $commit
        working_tree_dirty = ($workingTreeStatus.Count -gt 0)
        build_configuration = 'Release'
        preset = $Preset
        sample_count = $SampleCount
        cases = $cases
        operations = $operations
        warmup_count = $WarmupCount
        iterations = $Iterations
        cache_state = 'not controlled or claimed'
        codec_resident_budget_interpretation =
            'codec-controlled bulk allocations only; not a process RSS limit'
        environment = $environment
        build_commands = $buildCommands
        commands = $commands
        warmups = $warmups
        measurements = $measurements
        summaries = $summaries
        failures = $failures
    }
    $bundlePath = Join-Path $OutputDirectory 'benchmark.json'
    $bundle | ConvertTo-Json -Depth 20 |
        Set-Content -LiteralPath $bundlePath -Encoding UTF8

    foreach ($summary in $summaries) {
        Write-Host (
            '{0}/{1}: wall ms min={2:n3} median={3:n3} max={4:n3}' -f
            $summary.case,
            $summary.operation,
            $summary.wall_time_ms.min,
            $summary.wall_time_ms.median,
            $summary.wall_time_ms.max)
    }
    Write-Host "Benchmark evidence: $bundlePath"
    if ($failures.Count -gt 0) {
        throw "Production benchmark recorded $($failures.Count) failed subprocess(es)."
    }
}
finally {
    Pop-Location
}
