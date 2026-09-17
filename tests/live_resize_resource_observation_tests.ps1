param([string]$RepoRoot = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $RepoRoot 'scripts/profile-live-resize.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
# Load the sampler alone: never launch the interactive application from this test.
$definition = $ast.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-ResizeResourceSample'
}, $true)
if (!$definition) { throw 'Resource sampler missing.' }
Invoke-Expression $definition.Extent.Text
$script:counterMode = 'missing'
$script:expectedProcess = [Diagnostics.Process]::GetCurrentProcess()
function Get-CimInstance {
    param($ClassName, $Filter, $OperationTimeoutSec, $ErrorAction)
    if ($Filter -ne "Name LIKE 'pid_$($script:expectedProcess.Id)[_]%'" -or $OperationTimeoutSec -ne 1) {
        throw 'Wrong process scope or timeout.'
    }
    switch ($script:counterMode) {
        'error' { throw 'Counter denied.' }
        'valid' {
            [pscustomobject]@{ DedicatedUsage = 10; SharedUsage = 20; TotalCommitted = 40 }
            [pscustomobject]@{ DedicatedUsage = 30; SharedUsage = 50; TotalCommitted = 90 }
        }
        'partial' { [pscustomobject]@{ DedicatedUsage = 10; SharedUsage = $null; TotalCommitted = 40 } }
    }
}
foreach ($mode in @('missing', 'error', 'partial', 'valid')) {
    $script:counterMode = $mode
    $sample = Get-ResizeResourceSample -TargetProcess $script:expectedProcess -ElapsedMs 123
    if ($sample.elapsed_ms -ne 123 -or $sample.private_bytes -le 0 -or $sample.handle_count -le 0) {
        throw 'Process sample missing.'
    }
    if ($mode -eq 'valid') {
        if ($sample.gpu_status -ne 'available' -or $sample.gpu_instance_count -ne 2 -or
            $sample.gpu_dedicated_bytes -ne 40 -or $sample.gpu_shared_bytes -ne 70 -or $sample.gpu_committed_bytes -ne 130) {
            throw 'GPU adapter instance aggregation failed.'
        }
    } elseif ($sample.gpu_status -ne 'unavailable' -or $null -ne $sample.gpu_dedicated_bytes -or
        $null -ne $sample.gpu_shared_bytes -or $null -ne $sample.gpu_committed_bytes) {
        throw 'Missing/failed GPU counters must remain unknown, not zero.'
    }
    if ($mode -in @('error', 'partial') -and !$sample.gpu_error) { throw 'GPU error reason missing.' }
}
$script:expectedProcess.Dispose()
Write-Host 'Live resize resource observation tests passed.'
