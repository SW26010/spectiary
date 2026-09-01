[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot,

    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

. (Join-Path $RepoRoot 'scripts\lib\portable_pe_helpers.ps1')
. (Join-Path $RepoRoot 'tests\helpers\contract_test_support.ps1')
. (Join-Path $RepoRoot 'tests\helpers\pe_import_fixture.ps1')

$absoluteBuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$fixtureRoot = Join-Path $absoluteBuildDirectory '.portable-pe-contract'
[void][IO.Directory]::CreateDirectory($fixtureRoot)

function Assert-PeFixtureContract {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    Assert-StaticCfitsioPeImports `
        -ExecutablePath $Path `
        -Description $Description
}

function Test-PeAssertionIsReachable {
    param(
        [Parameter(Mandatory = $true)]
        [Management.Automation.Language.CommandAst]$Command
    )

    $ancestor = $Command.Parent
    while ($null -ne $ancestor) {
        if ($ancestor -is
                [Management.Automation.Language.FunctionDefinitionAst] -or
            $ancestor -is
                [Management.Automation.Language.ScriptBlockExpressionAst]) {
            return $false
        }
        if ($ancestor -is [Management.Automation.Language.IfStatementAst]) {
            foreach ($clause in $ancestor.Clauses) {
                $body = $clause.Item2.Extent
                if ($Command.Extent.StartOffset -ge $body.StartOffset -and
                    $Command.Extent.EndOffset -le $body.EndOffset -and
                    $clause.Item1.Extent.Text.Trim() -match
                        '^\(?\s*\$?false\s*\)?$') {
                    return $false
                }
            }
        }
        $ancestor = $ancestor.Parent
    }
    return $true
}

function Assert-PortableVerifierPeIntegration {
    param([Parameter(Mandatory = $true)] [string]$Text)

    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput(
        $Text,
        [ref]$tokens,
        [ref]$errors)
    Assert-ContractRule `
        -Condition ($errors.Count -eq 0) `
        -Id 'SF-PE-VERIFIER-INTEGRATION' `
        -Message 'Portable verifier must remain valid PowerShell.'
    $assertions = @(
        $ast.FindAll(
            {
                param($node)
                $node -is [Management.Automation.Language.CommandAst] -and
                $node.GetCommandName() -ceq 'Assert-StaticCfitsioPeImports'
            },
            $true) |
            Where-Object { Test-PeAssertionIsReachable -Command $_ }
    )
    $assertionText = @($assertions | ForEach-Object { $_.Extent.Text })
    Assert-ContractRule `
        -Condition (
            $assertions.Count -eq 2 -and
            @($assertionText | Where-Object {
                $_ -match '(?m)-ExecutablePath\s+\$BuildExecutable(?:\s|$)'
            }).Count -eq 1 -and
            @($assertionText | Where-Object {
                $_ -match '(?m)-ExecutablePath\s+\$packageExecutable(?:\s|$)'
            }).Count -eq 1
        ) `
        -Id 'SF-PE-VERIFIER-INTEGRATION' `
        -Message 'Portable verifier must inspect both the build and packaged PE imports.'
}

function New-ValidatedPeFixture {
    param(
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [AllowEmptyString()]
        [string]$RegularImport,
        [Parameter(Mandatory = $true)] [AllowEmptyString()]
        [string]$DelayImport
    )

    $path = Join-Path $fixtureRoot "$Name.exe"
    New-PeImportFixture `
        -Path $path `
        -RegularImport $RegularImport `
        -DelayImport $DelayImport
    $imports = @(Get-PeImportedDllNames -Path $path)
    foreach ($expectedImport in @($RegularImport, $DelayImport)) {
        if (-not [string]::IsNullOrEmpty($expectedImport)) {
            Assert-ContractRule `
                -Condition ($imports -ccontains $expectedImport) `
                -Id 'SF-HARNESS-FIXTURE' `
                -Message "$Name fixture did not expose $expectedImport."
        }
    }
    return $path
}

try {
    $portableVerifierPath = Join-Path $RepoRoot 'scripts\verify-portable.ps1'
    $portableVerifierText = Get-Content -Raw -LiteralPath $portableVerifierPath
    $baselineValidation = {
        $baselinePath = New-ValidatedPeFixture `
            -Name 'baseline' `
            -RegularImport 'KERNEL32.dll' `
            -DelayImport ''
        Assert-PeFixtureContract `
            -Path $baselinePath `
            -Description 'Baseline portable executable'
        Assert-PortableVerifierPeIntegration -Text $portableVerifierText
    }

    $mutationCases = @()
    foreach ($kind in @('regular', 'delay')) {
        foreach ($dllName in @(
            'cfitsio.dll',
            'libcfitsio.dll',
            'vendor\cfitsio.dll',
            'libcurl.dll',
            'bzip2.dll'
        )) {
            $caseKind = $kind
            $caseDllName = $dllName
            $expectedId = if ($caseDllName -match '[\\/]') {
                'SF-PE-IMPORTS-BASENAME'
            }
            else {
                'SF-PE-IMPORTS-STATIC'
            }
            $mutationCases += [pscustomobject]@{
                Description = "Portable PE rejects $caseKind import $caseDllName"
                ExpectedId = $expectedId
                Validate = {
                    $regularImport = if ($caseKind -ceq 'regular') {
                        $caseDllName
                    }
                    else {
                        'KERNEL32.dll'
                    }
                    $delayImport = if ($caseKind -ceq 'delay') {
                        $caseDllName
                    }
                    else {
                        ''
                    }
                    $fixturePath = New-ValidatedPeFixture `
                        -Name ("mutation-$caseKind-" +
                            $caseDllName.Replace('.', '-').Replace('\', '-').Replace('/', '-')) `
                        -RegularImport $regularImport `
                        -DelayImport $delayImport
                    Assert-PeFixtureContract `
                        -Path $fixturePath `
                        -Description 'Mutated portable executable'
                }.GetNewClosure()
            }
        }
    }
    $mutationCases += [pscustomobject]@{
        Description = 'Portable PE rejects an empty import table'
        ExpectedId = 'SF-PE-IMPORTS-EMPTY'
        Validate = {
            $fixturePath = New-ValidatedPeFixture `
                -Name 'mutation-empty' `
                -RegularImport '' `
                -DelayImport ''
            Assert-PeFixtureContract `
                -Path $fixturePath `
                -Description 'Mutated portable executable'
        }
    }
    $mutationCases += [pscustomobject]@{
        Description = 'Portable verifier cannot skip packaged PE inspection'
        ExpectedId = 'SF-PE-VERIFIER-INTEGRATION'
        Validate = {
            $mutatedText = [regex]::Replace(
                $portableVerifierText,
                '(?ms)^    Assert-StaticCfitsioPeImports `\r?\n' +
                    '        -ExecutablePath \$packageExecutable `\r?\n' +
                    '        -Description [^\r\n]+\r?\n',
                '',
                1)
            Assert-ContractRule `
                -Condition ($mutatedText -cne $portableVerifierText) `
                -Id 'SF-HARNESS-MUTATION' `
                -Message 'Packaged PE integration mutation did not change the verifier.'
            Assert-PortableVerifierPeIntegration -Text $mutatedText
        }
    }
    $mutationCases += [pscustomobject]@{
        Description = 'Portable verifier cannot hide packaged PE inspection in if(false)'
        ExpectedId = 'SF-PE-VERIFIER-INTEGRATION'
        Validate = {
            $pattern = '(?ms)^    Assert-StaticCfitsioPeImports `\r?\n' +
                '        -ExecutablePath \$packageExecutable `\r?\n' +
                '        -Description [^\r\n]+\r?\n'
            $evaluator = [Text.RegularExpressions.MatchEvaluator]{
                param($match)
                $caseNewline = if ($match.Value.Contains("`r`n")) {
                    "`r`n"
                }
                else {
                    "`n"
                }
                $body = @(
                    [regex]::Split($match.Value.TrimEnd("`r", "`n"), '\r?\n') |
                        ForEach-Object { '    ' + $_ }
                ) -join $caseNewline
                return '    if ($false) {' + $caseNewline +
                    $body + $caseNewline + '    }' + $caseNewline
            }
            $mutatedText = [regex]::new($pattern).Replace(
                $portableVerifierText,
                $evaluator,
                1)
            Assert-ContractRule `
                -Condition ($mutatedText -cne $portableVerifierText) `
                -Id 'SF-HARNESS-MUTATION' `
                -Message 'Unreachable PE integration mutation did not change the verifier.'
            Assert-PortableVerifierPeIntegration -Text $mutatedText
        }
    }

    Invoke-ContractMutationTable `
        -Cases $mutationCases `
        -ValidateBaseline $baselineValidation
}
finally {
    $resolvedFixtureRoot = [IO.Path]::GetFullPath($fixtureRoot)
    Assert-ContractRule `
        -Condition ($resolvedFixtureRoot.StartsWith(
            $absoluteBuildDirectory + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) `
        -Id 'SF-HARNESS-CLEANUP' `
        -Message 'Refusing to clean a PE fixture outside the configured build directory.'
    if (Test-Path -LiteralPath $resolvedFixtureRoot) {
        Remove-Item -LiteralPath $resolvedFixtureRoot -Recurse -Force
    }
}

Write-Host 'Portable PE import contract and mutation table passed.'
