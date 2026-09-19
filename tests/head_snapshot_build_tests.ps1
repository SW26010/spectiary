[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot
)

$ErrorActionPreference = 'Stop'

$headWrapperPath = Join-Path $RepoRoot 'scripts\build-portable-from-head.ps1'
$childBuilderFixturePath = Join-Path `
    $RepoRoot `
    'tests\fixtures\head_snapshot_build_portable.ps1'
foreach ($requiredPath in @($headWrapperPath, $childBuilderFixturePath)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required HEAD snapshot test source is missing: $requiredPath"
    }
}

$testRoot = Join-Path `
    ([IO.Path]::GetTempPath()) `
    "spectiary-head-snapshot-$PID-$([Guid]::NewGuid().ToString('N'))"
try {
    $fixtureScriptsRoot = Join-Path $testRoot 'scripts'
    New-Item -ItemType Directory -Path $fixtureScriptsRoot -Force | Out-Null
    Copy-Item `
        -LiteralPath $headWrapperPath `
        -Destination (Join-Path $fixtureScriptsRoot 'build-portable-from-head.ps1')
    Copy-Item `
        -LiteralPath $childBuilderFixturePath `
        -Destination (Join-Path $fixtureScriptsRoot 'build-portable.ps1')
    Set-Content `
        -LiteralPath (Join-Path $testRoot 'tracked-sentinel.txt') `
        -Value 'committed' `
        -Encoding ASCII

    & git -C $testRoot init --quiet
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not initialize the HEAD snapshot Git fixture.'
    }
    & git -C $testRoot config user.name 'Spectiary Tests'
    & git -C $testRoot config user.email 'spectiary-tests@example.invalid'
    & git -C $testRoot add -- .

    $hostileHooksRoot = Join-Path $testRoot '.git\hostile-hooks'
    $emptyHooksRoot = Join-Path $testRoot '.git\isolated-hooks'
    $hookSentinelPath = Join-Path $testRoot '.git\hostile-hook-ran'
    New-Item -ItemType Directory -Path $hostileHooksRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $emptyHooksRoot -Force | Out-Null
    @'
#!/bin/sh
printf invoked > "$SPECTIARY_HOOK_SENTINEL"
exit 1
'@ |
        Set-Content `
            -LiteralPath (Join-Path $hostileHooksRoot 'pre-commit') `
            -Encoding ASCII

    $injectedGitEnvironment = [ordered]@{
        GIT_CONFIG_COUNT = '3'
        GIT_CONFIG_KEY_0 = 'commit.gpgSign'
        GIT_CONFIG_VALUE_0 = 'true'
        GIT_CONFIG_KEY_1 = 'gpg.program'
        GIT_CONFIG_VALUE_1 = (Join-Path $testRoot 'missing-gpg-program.exe')
        GIT_CONFIG_KEY_2 = 'core.hooksPath'
        GIT_CONFIG_VALUE_2 = $hostileHooksRoot
        SPECTIARY_HOOK_SENTINEL = $hookSentinelPath
    }
    $previousGitEnvironment = [ordered]@{}
    foreach ($entry in $injectedGitEnvironment.GetEnumerator()) {
        $previousGitEnvironment[$entry.Key] = [Environment]::GetEnvironmentVariable(
            $entry.Key,
            'Process')
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
    }
    try {
        & git `
            -c commit.gpgSign=false `
            -c "core.hooksPath=$emptyHooksRoot" `
            -C $testRoot `
            commit `
            --quiet `
            -m 'head snapshot fixture'
    }
    finally {
        foreach ($entry in $previousGitEnvironment.GetEnumerator()) {
            [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
        }
    }
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not commit the HEAD snapshot Git fixture.'
    }
    if (Test-Path -LiteralPath $hookSentinelPath) {
        throw 'HEAD snapshot fixture executed an inherited Git hook.'
    }
    $fixtureRevision = (& git -C $testRoot rev-parse HEAD).Trim()

    Set-Content `
        -LiteralPath (Join-Path $testRoot 'tracked-sentinel.txt') `
        -Value 'dirty' `
        -Encoding ASCII
    Set-Content `
        -LiteralPath (Join-Path $testRoot 'untracked-sentinel.txt') `
        -Value 'untracked' `
        -Encoding ASCII
    $workingPackageRoot = Join-Path $testRoot 'dist\Spectiary-portable'
    New-Item -ItemType Directory -Path $workingPackageRoot -Force | Out-Null
    $workingPackageSentinel = Join-Path $workingPackageRoot 'preserve.txt'
    Set-Content -LiteralPath $workingPackageSentinel -Value 'preserve' -Encoding ASCII

    & powershell `
        -NoProfile `
        -ExecutionPolicy Bypass `
        -File (Join-Path $fixtureScriptsRoot 'build-portable-from-head.ps1') `
        -PackageName 'Spectiary-portable'
    if ($LASTEXITCODE -ne 0) {
        throw "HEAD snapshot integration fixture failed with exit code $LASTEXITCODE."
    }

    $invocationPath = Join-Path $testRoot 'dist\head\head-wrapper-invocation.json'
    if (-not (Test-Path -LiteralPath $invocationPath -PathType Leaf)) {
        throw 'HEAD wrapper did not invoke the child builder inside the snapshot.'
    }
    $invocation = Get-Content -Raw -LiteralPath $invocationPath | ConvertFrom-Json
    if ($invocation.source_mode -isnot [string] -or
        $invocation.source_mode -cne 'head' -or
        $invocation.source_revision -isnot [string] -or
        $invocation.source_revision -cne $fixtureRevision) {
        throw 'HEAD wrapper did not pass the frozen HEAD source contract to its child.'
    }
    if ($invocation.preset_was_bound -ne $false -or
        $invocation.configuration_was_bound -ne $false -or
        $invocation.preset -cne 'snapshot-owned-portable-preset' -or
        $invocation.configuration -cne 'SnapshotRelease' -or
        $invocation.package_name -cne 'Spectiary-portable') {
        throw 'HEAD wrapper did not leave preset and configuration defaults to the snapshot-owned builder.'
    }
    $legacyMetadataPath = Join-Path `
        $testRoot `
        'dist\head\Spectiary-portable\spectiary_build_metadata.json'
    if (-not (Test-Path -LiteralPath $legacyMetadataPath -PathType Leaf) -or
        $invocation.child_schema_version -ne 3) {
        throw 'HEAD wrapper did not accept the child builder generation-owned legacy output.'
    }
    if (Test-Path -LiteralPath (
        Join-Path $testRoot 'dist\head\Spectiary-portable\spectiary_metadata.json'
    )) {
        throw 'HEAD snapshot fixture unexpectedly produced current-generation metadata.'
    }
    if ($invocation.tracked_value -cne 'committed') {
        throw 'HEAD wrapper child did not observe committed tracked content.'
    }
    if (Test-Path -LiteralPath $invocation.snapshot_root) {
        throw "HEAD wrapper did not clean its temporary snapshot: $($invocation.snapshot_root)"
    }
    if (-not (Test-Path -LiteralPath $workingPackageSentinel -PathType Leaf)) {
        throw 'HEAD wrapper replaced the working-tree package.'
    }
    if ((Get-Content -Raw -LiteralPath (Join-Path $testRoot 'tracked-sentinel.txt')).Trim() -cne
        'dirty') {
        throw 'HEAD wrapper modified the caller working tree.'
    }
    if (-not (Test-Path -LiteralPath (Join-Path $testRoot 'untracked-sentinel.txt'))) {
        throw 'HEAD wrapper removed the caller untracked file.'
    }
}
finally {
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}

Write-Output 'HEAD snapshot build tests passed'
