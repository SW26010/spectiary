[CmdletBinding(SupportsShouldProcess = $true)]
param()

$ErrorActionPreference = 'Stop'

function Invoke-GitCapture {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    $output = @(& git -C $repoRoot @Arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        $detail = ($output | ForEach-Object { [string]$_ }) -join [Environment]::NewLine
        throw "git $($Arguments -join ' ') failed with exit code $LASTEXITCODE.`n$detail"
    }
    return @($output | ForEach-Object { [string]$_ })
}

function Resolve-Commit {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Revision
    )

    $output = @(Invoke-GitCapture -Arguments @('rev-parse', '--verify', "$Revision^{commit}"))
    if ($output.Count -ne 1 -or $output[0] -cnotmatch '^[0-9a-f]{40}$') {
        throw "Could not resolve '$Revision' to a full commit object ID."
    }
    return $output[0]
}

$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $scriptRoot '..')).Path

$insideWorkTree = @(Invoke-GitCapture -Arguments @('rev-parse', '--is-inside-work-tree'))
if ($insideWorkTree.Count -ne 1 -or $insideWorkTree[0] -cne 'true') {
    throw 'release-tag.ps1 must run from a Git working tree.'
}

$branch = @(Invoke-GitCapture -Arguments @('symbolic-ref', '--quiet', '--short', 'HEAD'))
if ($branch.Count -ne 1 -or $branch[0] -cne 'master') {
    $actual = if ($branch.Count -eq 1) { $branch[0] } else { '<detached>' }
    throw "Release tags may only be created from local master; current branch is '$actual'."
}

$status = @(Invoke-GitCapture -Arguments @('status', '--porcelain=v1', '--untracked-files=normal'))
if ($status.Count -ne 0) {
    throw 'The working tree must be clean before creating a release tag.'
}

$cmakeListsPath = Join-Path $repoRoot 'CMakeLists.txt'
$cmakeLists = Get-Content -Raw -LiteralPath $cmakeListsPath
$versionMatch = [regex]::Match(
    $cmakeLists,
    '(?m)^\s*project\s*\(\s*SpecForge\s+VERSION\s+((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*))\b'
)
if (-not $versionMatch.Success) {
    throw 'Could not resolve a stable MAJOR.MINOR.PATCH SpecForge VERSION from CMakeLists.txt.'
}
$version = $versionMatch.Groups[1].Value
$tag = "v$version"

[void](Invoke-GitCapture -Arguments @('fetch', '--no-tags', 'origin', '+refs/heads/master:refs/remotes/origin/master'))
$headCommit = Resolve-Commit -Revision 'HEAD'
$originMasterCommit = Resolve-Commit -Revision 'refs/remotes/origin/master'
if ($headCommit -cne $originMasterCommit) {
    throw "Local master is not exactly origin/master. HEAD=$headCommit origin/master=$originMasterCommit"
}

& git -C $repoRoot show-ref --verify --quiet "refs/tags/$tag"
if ($LASTEXITCODE -eq 0) {
    throw "Local tag '$tag' already exists."
}
if ($LASTEXITCODE -ne 1) {
    throw "Could not determine whether local tag '$tag' exists."
}

$remoteTag = @(& git -C $repoRoot ls-remote --exit-code --tags origin "refs/tags/$tag" 2>$null)
if ($LASTEXITCODE -eq 0) {
    throw "Remote tag '$tag' already exists."
}
if ($LASTEXITCODE -ne 2) {
    throw "Could not determine whether remote tag '$tag' exists."
}

$description = "create annotated tag $tag at $headCommit and push it to origin"
if (-not $PSCmdlet.ShouldProcess('origin', $description)) {
    return
}

[void](Invoke-GitCapture -Arguments @('tag', '-a', $tag, $headCommit, '-m', "SpecForge $tag"))
try {
    [void](Invoke-GitCapture -Arguments @('push', 'origin', "refs/tags/${tag}:refs/tags/${tag}"))
}
catch {
    $remoteAfterFailure = @(& git -C $repoRoot ls-remote --exit-code --tags origin "refs/tags/$tag" 2>$null)
    if ($LASTEXITCODE -eq 2) {
        & git -C $repoRoot tag --delete $tag *> $null
    }
    throw
}

Write-Host "Release tag $tag now points to $headCommit and has been pushed to origin."
Write-Host 'GitHub Actions will build, verify, and publish the corresponding GitHub Release.'
