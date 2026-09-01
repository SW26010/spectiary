[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot,

    [Parameter(Mandatory = $false)]
    [string]$DebugBuildDirectory = '',

    [Parameter(Mandatory = $false)]
    [string]$ReleaseBuildDirectory = '',

    [Parameter(Mandatory = $false)]
    [ValidateSet('WorkflowStructure', 'ConfiguredBuild')]
    [string]$ContractArea = 'WorkflowStructure'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

. (Join-Path $RepoRoot 'tests\helpers\contract_test_support.ps1')

function Get-ReleaseContractRuleId {
    param([Parameter(Mandatory = $true)] [string]$Message)

    switch -Regex ($Message) {
        'automatically on push' {
            return 'SF-WF-PUSH-TRIGGER'
        }
        'automatically on pull_request' {
            return 'SF-WF-PULL-REQUEST-TRIGGER'
        }
        'required release build job cannot be conditional' {
            return 'SF-WF-RELEASE-JOB-POLICY'
        }
        'native/headless required job cannot be conditional' {
            return 'SF-WF-AUTOMATION-JOB-POLICY'
        }
        'Static Release FITS verification cannot be conditional' {
            return 'SF-WF-STATIC-STEP-POLICY'
        }
        'Configure Ninja/MSVC static Release cannot be conditional' {
            return 'SF-WF-AUTOMATION-RELEASE-CONFIG-POLICY'
        }
        'Workflow must define active step ''Configure Ninja/MSVC static Release''' {
            return 'SF-WF-AUTOMATION-RELEASE-CONFIG-STEP'
        }
        'Run required static Release CTest gate must run the expected configured' {
            return 'SF-WF-AUTOMATION-RELEASE-GATE'
        }
        'Build native automation targets must fail through one directly reachable' {
            return 'SF-WF-AUTOMATION-DEBUG-EXIT'
        }
        'Static Release FITS workflow step is not valid PowerShell' {
            return 'SF-WF-STATIC-SCRIPT-SYNTAX'
        }
        'Static Release FITS verification must actively invoke the build wrapper three times' {
            return 'SF-WF-STATIC-WRAPPER-COUNT'
        }
        'Static Release workflow must actively invoke exactly two CTest commands' {
            return 'SF-WF-STATIC-CTEST-COUNT'
        }
        'Every FITS verification build command must use the static preset' {
            return 'SF-WF-STATIC-WRAPPER-ARGS'
        }
        'Static Release FITS verification command must fail through one directly reachable' {
            return 'SF-WF-STATIC-EXIT-GUARD'
        }
        'expected configured ci-headless graph|bounded CTest|resolved ci-headless label' {
            return 'SF-WF-AUTOMATION-GATE'
        }
        'required build wrapper|must build .* exactly once|perform configuration' {
            return 'SF-WF-AUTOMATION-BUILD'
        }
        'active run block|malformed run-block|not valid PowerShell' {
            return 'SF-WF-ACTIVE-SCRIPT'
        }
        'Release configured cache must use triplet' {
            return 'SF-CFG-RELEASE-TRIPLET'
        }
        'Debug configured cache must use triplet' {
            return 'SF-CFG-DEBUG-TRIPLET'
        }
        'Release C\+\+ compile rule must use|Static Release C\+\+ compile graph must not contain' {
            return 'SF-CFG-RELEASE-CRT'
        }
        'Debug C\+\+ compile rule must use' {
            return 'SF-CFG-DEBUG-CRT'
        }
        'specforge_loader_tests must preserve the executable exit code' {
            return 'SF-CFG-LOADER-EXIT-AUTHORITY'
        }
        'specforge_fits_file_reader_tests must preserve the executable exit code' {
            return 'SF-CFG-READER-EXIT-AUTHORITY'
        }
        'specforge_loader_tests must participate in the automatic ci-headless required gate' {
            return 'SF-CFG-LOADER-GATE'
        }
        'specforge_fits_file_reader_tests must participate in the automatic ci-headless required gate' {
            return 'SF-CFG-READER-GATE'
        }
        'Configured CTest graph must register specforge_loader_tests|specforge_loader_tests must (execute|be a configured executable)' {
            return 'SF-CFG-LOADER-GRAPH'
        }
        'Configured CTest graph must register specforge_fits_file_reader_tests|specforge_fits_file_reader_tests must (execute|be a configured executable)' {
            return 'SF-CFG-READER-GRAPH'
        }
        'specforge_release_artifacts_tests must be configured in the timed' {
            return 'SF-CFG-ARTIFACT-GATE'
        }
        'specforge_portable_pe_contract_tests must be configured in the timed' {
            return 'SF-CFG-PE-GATE'
        }
        'specforge_release_workflow_structure_contract_tests must be configured in the timed' {
            return 'SF-CFG-WORKFLOW-GATE'
        }
        'specforge_release_configured_build_contract_tests must be configured in the timed' {
            return 'SF-CFG-CONFIGURED-GATE'
        }
        'Spectrum loader entry point must unconditionally execute TestLoadsGzippedFitsSpectrum' {
            return 'SF-CFG-LOADER-GZIP-COVERAGE'
        }
        'Spectrum loader entry point must unconditionally execute TestLoadsFitsScalarTableSpectrum' {
            return 'SF-CFG-LOADER-SCALAR-COVERAGE'
        }
        'Spectrum loader entry point must unconditionally execute TestLoadsLimitedFitsImageSpectrum' {
            return 'SF-CFG-LOADER-IMAGE-COVERAGE'
        }
        'FITS reader entry point' {
            return 'SF-CFG-READER-COVERAGE'
        }
        'production FITS reader and loader sources' {
            return 'SF-CFG-PRODUCTION-SOURCES'
        }
        'Configured CTest graph must register .* exactly once' {
            return 'SF-CFG-CONTRACT-REGISTRATION'
        }
        'configured in the timed ci-headless required gate' {
            return 'SF-CFG-REQUIRED-GATE'
        }
        'immediate failure guard|nonzero executable exit|directly reachable top-level throw' {
            return 'SF-WF-EXIT-AUTHORITY'
        }
        'conditional or allowed to continue on error' {
            return 'SF-WF-EXECUTION-POLICY'
        }
        default {
            return $(if ($ContractArea -ceq 'ConfiguredBuild') {
                'SF-CFG-STRUCTURE'
            }
            else {
                'SF-WF-STRUCTURE'
            })
        }
    }
}

function Assert-True {
    param(
        [Parameter(Mandatory = $true)] [bool]$Condition,
        [Parameter(Mandatory = $true)] [string]$Message
    )

    Assert-ContractRule `
        -Condition $Condition `
        -Id (Get-ReleaseContractRuleId -Message $Message) `
        -Message $Message
}

function Assert-ValidationFails {
    param(
        [Parameter(Mandatory = $true)] [string]$Description,
        [Parameter(Mandatory = $true)] [string]$ExpectedId,
        [Parameter(Mandatory = $true)] [scriptblock]$Operation
    )

    Assert-ContractMutation `
        -Description $Description `
        -ExpectedId $ExpectedId `
        -ValidateBaseline { Invoke-SelectedContractValidation @contractInputs } `
        -ValidateMutation $Operation
}

function Get-YamlJobBody {
    param(
        [Parameter(Mandatory = $true)] [string]$Text,
        [Parameter(Mandatory = $true)] [string]$JobId
    )

    $pattern = '(?ms)^  ' + [regex]::Escape($JobId) +
        ':\r?\n(?<body>.*?)(?=^  [A-Za-z0-9_-]+:\r?\n|\z)'
    $match = [regex]::Match($Text, $pattern)
    Assert-True `
        -Condition $match.Success `
        -Message "Workflow must define job '$JobId'."
    return $match.Groups['body'].Value
}

function Get-YamlTopLevelBody {
    param(
        [Parameter(Mandatory = $true)] [string]$Text,
        [Parameter(Mandatory = $true)] [string]$Name
    )

    $pattern = '(?ms)^' + [regex]::Escape($Name) +
        ':\r?\n(?<body>.*?)(?=^[A-Za-z0-9_-]+:\r?\n|\z)'
    $match = [regex]::Match($Text, $pattern)
    Assert-True `
        -Condition $match.Success `
        -Message "Workflow must define active top-level '$Name'."
    return $match.Groups['body'].Value
}

function Get-YamlStepBody {
    param(
        [Parameter(Mandatory = $true)] [string]$Text,
        [Parameter(Mandatory = $true)] [string]$StepName
    )

    $pattern = '(?ms)^      - name: ' + [regex]::Escape($StepName) +
        '\r?\n(?<body>.*?)(?=^      - name: |\z)'
    $match = [regex]::Match($Text, $pattern)
    Assert-True `
        -Condition $match.Success `
        -Message "Workflow must define active step '$StepName'."
    return $match.Groups['body'].Value
}

function Get-YamlStepRunScript {
    param(
        [Parameter(Mandatory = $true)] [string]$Text,
        [Parameter(Mandatory = $true)] [string]$StepName
    )

    $stepBody = Get-YamlStepBody -Text $Text -StepName $StepName
    $runMatch = [regex]::Match($stepBody, '(?m)^        run: \|\r?\n')
    Assert-True `
        -Condition $runMatch.Success `
        -Message "Workflow step '$StepName' must use an active run block."
    $runBlock = $stepBody.Substring(
        $runMatch.Index + $runMatch.Length)
    $scriptLines = [System.Collections.Generic.List[string]]::new()
    foreach ($line in [regex]::Split($runBlock, '\r?\n')) {
        if ($line.Length -eq 0) {
            [void]$scriptLines.Add('')
            continue
        }
        Assert-True `
            -Condition $line.StartsWith('          ') `
            -Message "Workflow step '$StepName' has malformed run-block indentation."
        [void]$scriptLines.Add($line.Substring(10))
    }
    return $scriptLines -join [Environment]::NewLine
}

function Get-PowerShellAst {
    param(
        [Parameter(Mandatory = $true)] [string]$Text,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseInput(
        $Text,
        [ref]$tokens,
        [ref]$errors)
    Assert-True `
        -Condition ($errors.Count -eq 0) `
        -Message "$Description is not valid PowerShell: $(@($errors | ForEach-Object { $_.Message }) -join ' | ')"
    return $ast
}

function Get-PowerShellCommands {
    param(
        [Parameter(Mandatory = $true)]
        [Management.Automation.Language.Ast]$Ast
    )

    $commands = @(
        $Ast.FindAll(
            {
                param($node)
                $node -is [Management.Automation.Language.CommandAst]
            },
            $true)
    )
    return @(
        $commands | Where-Object {
            $ancestor = $_.Parent
            $deferred = $false
            while ($null -ne $ancestor) {
                if ($ancestor -is
                        [Management.Automation.Language.FunctionDefinitionAst] -or
                    $ancestor -is
                        [Management.Automation.Language.ScriptBlockExpressionAst]) {
                    $deferred = $true
                    break
                }
                $ancestor = $ancestor.Parent
            }
            -not $deferred
        }
    )
}

function Get-TopLevelPowerShellCommands {
    param(
        [Parameter(Mandatory = $true)]
        [Management.Automation.Language.ScriptBlockAst]$Ast
    )

    $commands = [System.Collections.Generic.List[object]]::new()
    foreach ($statement in @($Ast.EndBlock.Statements)) {
        if ($statement -isnot [Management.Automation.Language.PipelineAst]) {
            continue
        }
        foreach ($element in @($statement.PipelineElements)) {
            if ($element -is [Management.Automation.Language.CommandAst]) {
                [void]$commands.Add($element)
            }
        }
    }
    return @($commands)
}

function Assert-CommandHasImmediateFailureGuard {
    param(
        [Parameter(Mandatory = $true)]
        [Management.Automation.Language.ScriptBlockAst]$Ast,
        [Parameter(Mandatory = $true)]
        [Management.Automation.Language.CommandAst]$Command,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    $statements = @($Ast.EndBlock.Statements)
    $pipeline = $Command.Parent
    while ($null -ne $pipeline -and
        $pipeline -isnot [Management.Automation.Language.PipelineAst]) {
        $pipeline = $pipeline.Parent
    }
    $index = -1
    for ($candidate = 0; $candidate -lt $statements.Count; ++$candidate) {
        if ([object]::ReferenceEquals($statements[$candidate], $pipeline)) {
            $index = $candidate
            break
        }
    }
    Assert-True `
        -Condition ($index -ge 0 -and $index + 1 -lt $statements.Count) `
        -Message "$Description must be a top-level command followed by an exit-code guard."
    $guard = $statements[$index + 1]
    $guardStatements = @()
    if ($guard -is [Management.Automation.Language.IfStatementAst]) {
        if ($guard.Clauses.Count -eq 1) {
            $guardStatements = @($guard.Clauses[0].Item2.Statements)
        }
    }
    Assert-True `
        -Condition (
            $guard -is [Management.Automation.Language.IfStatementAst] -and
            $guard.Extent.Text -match
                '(?s)^if\s*\(\s*\$LASTEXITCODE\s+-ne\s+0\s*\)' -and
            $guardStatements.Count -eq 1 -and
            ($guardStatements[0] -is
                [Management.Automation.Language.ThrowStatementAst] -or
             $guardStatements[0] -is
                [Management.Automation.Language.ExitStatementAst])
        ) `
        -Message "$Description must fail through one directly reachable top-level throw or exit when LASTEXITCODE is nonzero."
}

function Get-CommandElementTexts {
    param(
        [Parameter(Mandatory = $true)]
        [Management.Automation.Language.CommandAst]$Command
    )

    return @(
        $Command.CommandElements | ForEach-Object {
            if ($_ -is
                [Management.Automation.Language.StringConstantExpressionAst]) {
                $_.Value
            }
            elseif ($_ -is
                [Management.Automation.Language.CommandParameterAst]) {
                '-' + $_.ParameterName
            }
            else {
                $_.Extent.Text
            }
        }
    )
}

function Get-CommandArgumentValue {
    param(
        [Parameter(Mandatory = $true)] [string[]]$Elements,
        [Parameter(Mandatory = $true)] [string]$Name
    )

    $indexes = @(
        for ($index = 0; $index -lt $Elements.Count; ++$index) {
            if ($Elements[$index] -ceq $Name) {
                $index
            }
        }
    )
    if ($indexes.Count -ne 1 -or $indexes[0] + 1 -ge $Elements.Count) {
        return $null
    }
    return $Elements[$indexes[0] + 1]
}

function Remove-CppComments {
    param([Parameter(Mandatory = $true)] [string]$Text)

    $withoutBlockComments = [regex]::Replace(
        $Text,
        '(?s)/\*.*?\*/',
        '')
    return [regex]::Replace(
        $withoutBlockComments,
        '(?m)//.*$',
        '')
}

function Remove-InactiveCppBlocks {
    param([Parameter(Mandatory = $true)] [string]$Text)

    $result = [System.Collections.Generic.List[string]]::new()
    $inactiveDepth = 0
    foreach ($line in [regex]::Split($Text, '\r?\n')) {
        if ($line -match '^\s*#\s*if\s+(?:0|false)\s*(?://.*)?$') {
            ++$inactiveDepth
            continue
        }
        if ($line -match '^\s*#\s*if\b') {
            if ($inactiveDepth -gt 0) {
                ++$inactiveDepth
            }
            else {
                [void]$result.Add($line)
            }
            continue
        }
        if ($line -match '^\s*#\s*endif\b') {
            if ($inactiveDepth -gt 0) {
                --$inactiveDepth
            }
            else {
                [void]$result.Add($line)
            }
            continue
        }
        if ($inactiveDepth -eq 0) {
            [void]$result.Add($line)
        }
    }
    Assert-True `
        -Condition ($inactiveDepth -eq 0) `
        -Message 'C++ test source contains an unterminated inactive preprocessor block.'
    return $result -join [Environment]::NewLine
}

function Get-CppFunctionBody {
    param(
        [Parameter(Mandatory = $true)] [string]$Text,
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    $activeText = Remove-InactiveCppBlocks `
        -Text (Remove-CppComments -Text $Text)
    $signature = if ($Name -ceq 'main') {
        '^int\s+main\s*\(\s*\)'
    }
    else {
        '^void\s+' + [regex]::Escape($Name) + '\s*\([^)]*\)'
    }
    $match = [regex]::Match(
        $activeText,
        '(?ms)' + $signature + '\s*\{\r?\n(?<body>.*?)^\}')
    Assert-True `
        -Condition $match.Success `
        -Message "$Description must define active function '$Name'."
    return $match.Groups['body'].Value
}

function Test-CppHasTopLevelInvocation {
    param(
        [Parameter(Mandatory = $true)] [string]$Body,
        [Parameter(Mandatory = $true)] [string]$Name,
        [string]$ArgumentsPattern = '\s*',
        [int]$ExpectedBraceDepth = 0
    )

    $pattern = '\b' + [regex]::Escape($Name) + '\s*\(\s*' +
        $ArgumentsPattern + '\s*\)\s*;'
    foreach ($match in [regex]::Matches($Body, $pattern)) {
        $depth = 0
        $quote = [char]0
        $escaped = $false
        for ($index = 0; $index -lt $match.Index; ++$index) {
            $character = $Body[$index]
            if ($quote -ne [char]0) {
                if ($escaped) {
                    $escaped = $false
                }
                elseif ($character -eq '\') {
                    $escaped = $true
                }
                elseif ($character -eq $quote) {
                    $quote = [char]0
                }
                continue
            }
            if ($character -eq '"' -or $character -eq "'") {
                $quote = $character
            }
            elseif ($character -eq '{') {
                ++$depth
            }
            elseif ($character -eq '}') {
                --$depth
            }
        }
        if ($depth -eq $ExpectedBraceDepth) {
            return $true
        }
    }
    return $false
}

function Get-CTestPropertyValue {
    param(
        [Parameter(Mandatory = $true)] [object]$Test,
        [Parameter(Mandatory = $true)] [string]$Name
    )

    $properties = @(
        @($Test.properties) | Where-Object { $_.name -ceq $Name }
    )
    if ($properties.Count -ne 1) {
        return $null
    }
    return $properties[0].value
}

function Assert-ConfiguredTestExecutable {
    param(
        [Parameter(Mandatory = $true)] [object]$CTestModel,
        [Parameter(Mandatory = $true)] [string]$BuildGraphText,
        [Parameter(Mandatory = $true)] [string]$BuildDirectory,
        [Parameter(Mandatory = $true)] [string]$Name,
        [Parameter(Mandatory = $true)] [string]$SourceObject
    )

    $tests = @(
        @($CTestModel.tests) | Where-Object { $_.name -ceq $Name }
    )
    Assert-True `
        -Condition ($tests.Count -eq 1) `
        -Message "Configured CTest graph must register $Name exactly once."
    $expectedExecutable = [IO.Path]::GetFullPath(
        (Join-Path $BuildDirectory "$Name.exe"))
    $actualExecutable = [IO.Path]::GetFullPath([string]$tests[0].command[0])
    Assert-True `
        -Condition ($actualExecutable -ceq $expectedExecutable) `
        -Message "$Name must execute the configured real test binary."
    $passProperties = @(
        @($tests[0].properties) |
            Where-Object { $_.name -ceq 'PASS_REGULAR_EXPRESSION' }
    )
    Assert-True `
        -Condition ($passProperties.Count -eq 0) `
        -Message "$Name must preserve the executable exit code as the authoritative CTest result."
    $labels = @(Get-CTestPropertyValue -Test $tests[0] -Name 'LABELS')
    Assert-True `
        -Condition (
            $labels -ccontains 'ci-headless' -and
            $labels -ccontains 'required'
        ) `
        -Message "$Name must participate in the automatic ci-headless required gate."

    $rule = [regex]::Match(
        $BuildGraphText,
        '(?m)^build ' + [regex]::Escape("$Name.exe") +
            ': (?<body>[^\r\n]+)')
    Assert-True `
        -Condition (
            $rule.Success -and
            $rule.Groups['body'].Value.Contains($SourceObject) -and
            $rule.Groups['body'].Value.Contains('specforge_core.lib') -and
            $rule.Groups['body'].Value -match
                '(?:^|[\\/])(?:debug[\\/])?lib[\\/]cfitsio\.lib(?:\s|$)'
        ) `
        -Message "$Name must be a configured executable built from its source and linked through specforge_core to CFITSIO."
}

function Assert-ConfiguredBuildMode {
    param(
        [Parameter(Mandatory = $true)] [string]$CacheText,
        [Parameter(Mandatory = $true)] [string]$BuildGraphText,
        [Parameter(Mandatory = $true)] [string]$Configuration,
        [Parameter(Mandatory = $true)] [string]$Triplet,
        [Parameter(Mandatory = $true)] [string]$RuntimeFlag
    )

    Assert-True `
        -Condition (
            $CacheText -match ('(?m)^CMAKE_BUILD_TYPE:STRING=' +
                [regex]::Escape($Configuration) + '\r?$') -and
            $CacheText -match ('(?m)^VCPKG_TARGET_TRIPLET:STRING=' +
                [regex]::Escape($Triplet) + '\r?$')
        ) `
        -Message "$Configuration configured cache must use triplet $Triplet."
    $compileFlagLines = @(
        [regex]::Matches(
            $BuildGraphText,
            '(?m)^  FLAGS = (?<flags>[^\r\n]*-std:c\+\+20[^\r\n]*)') |
            ForEach-Object { $_.Groups['flags'].Value }
    )
    Assert-True `
        -Condition ($compileFlagLines.Count -gt 0) `
        -Message "$Configuration Ninja graph must contain real C++ compile rules."
    foreach ($flags in $compileFlagLines) {
        Assert-True `
            -Condition ($flags -match (
                '(?:^|\s)' + [regex]::Escape($RuntimeFlag) + '(?:\s|$)')) `
            -Message "$Configuration C++ compile rule must use $RuntimeFlag."
    }
    if ($Configuration -ceq 'Release') {
        Assert-True `
            -Condition (@(
                $compileFlagLines | Where-Object {
                    $_ -match '(?:^|\s)-MDd?(?:\s|$)'
                }
            ).Count -eq 0) `
            -Message 'Static Release C++ compile graph must not contain /MD or /MDd.'
    }
}

function Assert-CMakeAndFitsTestContract {
    param(
        [Parameter(Mandatory = $true)] [string]$CTestJson,
        [Parameter(Mandatory = $true)] [string]$BuildGraphText,
        [Parameter(Mandatory = $true)] [string]$CacheText,
        [Parameter(Mandatory = $true)] [string]$BuildDirectory,
        [Parameter(Mandatory = $true)] [string]$Configuration,
        [Parameter(Mandatory = $true)] [string]$Triplet,
        [Parameter(Mandatory = $true)] [string]$RuntimeFlag,
        [Parameter(Mandatory = $true)] [string]$LoaderText,
        [Parameter(Mandatory = $true)] [string]$ReaderText
    )

    $ctestModel = $CTestJson | ConvertFrom-Json
    Assert-ConfiguredBuildMode `
        -CacheText $CacheText `
        -BuildGraphText $BuildGraphText `
        -Configuration $Configuration `
        -Triplet $Triplet `
        -RuntimeFlag $RuntimeFlag
    $coreRule = [regex]::Match(
        $BuildGraphText,
        '(?m)^build specforge_core\.lib: (?<body>[^\r\n]+)')
    Assert-True `
        -Condition (
            $coreRule.Success -and
            $coreRule.Groups['body'].Value.Contains(
                'src\domain\fits_file_reader.cpp.obj') -and
            $coreRule.Groups['body'].Value.Contains(
                'src\domain\fits_spectrum_loader.cpp.obj') -and
            $coreRule.Groups['body'].Value.Contains(
                'src\domain\spectrum_loader.cpp.obj')
        ) `
        -Message 'Configured specforge_core must compile the production FITS reader and loader sources.'

    Assert-ConfiguredTestExecutable `
        -CTestModel $ctestModel `
        -BuildGraphText $BuildGraphText `
        -BuildDirectory $BuildDirectory `
        -Name 'specforge_loader_tests' `
        -SourceObject 'tests\spectrum_loader_tests.cpp.obj'
    Assert-ConfiguredTestExecutable `
        -CTestModel $ctestModel `
        -BuildGraphText $BuildGraphText `
        -BuildDirectory $BuildDirectory `
        -Name 'specforge_fits_file_reader_tests' `
        -SourceObject 'tests\fits_file_reader_tests.cpp.obj'

    foreach ($releaseTestName in @(
        'specforge_release_artifacts_tests',
        'specforge_portable_pe_contract_tests',
        'specforge_release_workflow_structure_contract_tests',
        'specforge_release_configured_build_contract_tests'
    )) {
        $releaseTests = @(
            @($ctestModel.tests) |
                Where-Object { $_.name -ceq $releaseTestName }
        )
        Assert-True `
            -Condition ($releaseTests.Count -eq 1) `
            -Message "Configured CTest graph must register $releaseTestName exactly once."
        $labels = @(
            Get-CTestPropertyValue -Test $releaseTests[0] -Name 'LABELS'
        )
        $timeout = Get-CTestPropertyValue `
            -Test $releaseTests[0] `
            -Name 'TIMEOUT'
        Assert-True `
            -Condition (
                $labels -ccontains 'release' -and
                $labels -ccontains 'ci-headless' -and
                $labels -ccontains 'required' -and
                [double]$timeout -gt 0
            ) `
            -Message "$releaseTestName must be configured in the timed ci-headless required gate."
    }

    $readerBody = Get-CppFunctionBody `
        -Text $ReaderText `
        -Name 'TestPlainUnicodeAndMemoryBackedOpening' `
        -Description 'FITS reader tests'
    $readerMain = Get-CppFunctionBody `
        -Text $ReaderText `
        -Name 'main' `
        -Description 'FITS reader tests'
    Assert-True `
        -Condition (
            $readerBody.Contains('multi_hdu.fits') -and
            $readerBody.Contains('memory_backed.fits.gz') -and
            $readerBody.Contains('FitsSourceEncoding::Plain') -and
            $readerBody.Contains('FitsSourceEncoding::Gzip') -and
            (Test-CppHasTopLevelInvocation `
                -Body $readerMain `
                -Name 'TestPlainUnicodeAndMemoryBackedOpening' `
                -ArgumentsPattern 'temporary' `
                -ExpectedBraceDepth 1)
        ) `
        -Message 'FITS reader entry point must unconditionally execute the plain and memory-backed gzip CFITSIO case.'

    $loaderMain = Get-CppFunctionBody `
        -Text $LoaderText `
        -Name 'main' `
        -Description 'Spectrum loader tests'
    foreach ($loaderCase in @(
        [pscustomobject]@{
            Name = 'TestLoadsFitsScalarTableSpectrum'
            RequiredPath = '.fits'
        },
        [pscustomobject]@{
            Name = 'TestLoadsLimitedFitsImageSpectrum'
            RequiredPath = '.fits'
        },
        [pscustomobject]@{
            Name = 'TestLoadsGzippedFitsSpectrum'
            RequiredPath = '.fits.gz'
        }
    )) {
        $caseBody = Get-CppFunctionBody `
            -Text $LoaderText `
            -Name $loaderCase.Name `
            -Description 'Spectrum loader tests'
        Assert-True `
            -Condition (
                $caseBody.Contains($loaderCase.RequiredPath) -and
                $caseBody.Contains('LoadSpectrumSnapshotFromPath') -and
                (Test-CppHasTopLevelInvocation `
                    -Body $loaderMain `
                    -Name $loaderCase.Name)
            ) `
            -Message "Spectrum loader entry point must unconditionally execute $($loaderCase.Name) through the production loader."
    }
}

function Assert-CMakeMutationCannotSatisfyConfiguredGraph {
    param(
        [Parameter(Mandatory = $true)] [string]$Description,
        [Parameter(Mandatory = $true)] [string]$Body,
        [Parameter(Mandatory = $true)] [string]$BuildDirectory
    )

    $absoluteBuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
    $mutationRoot = Join-Path $absoluteBuildDirectory `
        ('.release-contract-mutation-' + [guid]::NewGuid().ToString('N'))
    $sourceDirectory = Join-Path $mutationRoot 'source'
    $fixtureBuildDirectory = Join-Path $mutationRoot 'build'
    [void][IO.Directory]::CreateDirectory($sourceDirectory)
    $cmakeText = @"
cmake_minimum_required(VERSION 3.24)
project(ReleaseContractMutation LANGUAGES NONE)
enable_testing()
add_custom_target(specforge_core)
$Body
"@
    $utf8 = [Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText(
        (Join-Path $sourceDirectory 'CMakeLists.txt'),
        $cmakeText,
        $utf8)
    try {
        $configureOutput = @(
            & cmake -S $sourceDirectory -B $fixtureBuildDirectory -G Ninja 2>&1
        )
        Assert-True `
            -Condition ($LASTEXITCODE -eq 0) `
            -Message "$Description fixture did not configure: $($configureOutput -join ' | ')"
        $fixtureCTestJson = @(
            & ctest --test-dir $fixtureBuildDirectory --show-only=json-v1
        ) -join [Environment]::NewLine
        Assert-True `
            -Condition ($LASTEXITCODE -eq 0) `
            -Message "$Description fixture CTest graph could not be read."
        $fixtureGraph = Get-Content -Raw -LiteralPath `
            (Join-Path $fixtureBuildDirectory 'build.ninja')
        Assert-ValidationFails `
            -Description $Description `
            -ExpectedId 'SF-CFG-LOADER-GRAPH' `
            -Operation {
                Assert-ConfiguredTestExecutable `
                    -CTestModel ($fixtureCTestJson | ConvertFrom-Json) `
                    -BuildGraphText $fixtureGraph `
                    -BuildDirectory $fixtureBuildDirectory `
                    -Name 'specforge_loader_tests' `
                    -SourceObject 'tests\spectrum_loader_tests.cpp.obj'
            }
    }
    finally {
        $resolvedMutationRoot = [IO.Path]::GetFullPath($mutationRoot)
        Assert-True `
            -Condition ($resolvedMutationRoot.StartsWith(
                $absoluteBuildDirectory + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase)) `
            -Message 'Refusing to clean a mutation fixture outside the configured build directory.'
        if (Test-Path -LiteralPath $resolvedMutationRoot) {
            Remove-Item -LiteralPath $resolvedMutationRoot -Recurse -Force
        }
    }
}

function Assert-CtestExitCodeRemainsAuthoritative {
    param(
        [Parameter(Mandatory = $true)] [string]$BuildDirectory
    )

    $absoluteBuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
    $fixtureRoot = Join-Path $absoluteBuildDirectory `
        ('.release-contract-exit-' + [guid]::NewGuid().ToString('N'))
    $sourceDirectory = Join-Path $fixtureRoot 'source'
    $fixtureBuildDirectory = Join-Path $fixtureRoot 'build'
    [void][IO.Directory]::CreateDirectory($sourceDirectory)
    $utf8 = [Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText(
        (Join-Path $sourceDirectory 'marker_then_failure.ps1'),
        "Write-Host 'MARKER_BEFORE_FAILURE'`r`nexit 1`r`n",
        $utf8)
    [IO.File]::WriteAllText(
        (Join-Path $sourceDirectory 'CMakeLists.txt'),
        @'
cmake_minimum_required(VERSION 3.24)
project(CTestExitContract LANGUAGES NONE)
enable_testing()
add_test(
    NAME marker_then_failure
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/marker_then_failure.ps1"
)
'@,
        $utf8)
    try {
        $configureOutput = @(
            & cmake -S $sourceDirectory -B $fixtureBuildDirectory -G Ninja 2>&1
        )
        Assert-True `
            -Condition ($LASTEXITCODE -eq 0) `
            -Message "CTest exit fixture did not configure: $($configureOutput -join ' | ')"
        $previousErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $ctestOutput = @(
                & ctest `
                    --test-dir $fixtureBuildDirectory `
                    --output-on-failure `
                    --no-tests=error `
                    -R '^marker_then_failure$' 2>&1
            ) -join [Environment]::NewLine
            $ctestExitCode = $LASTEXITCODE
        }
        finally {
            $ErrorActionPreference = $previousErrorActionPreference
        }
        Assert-True `
            -Condition (
                $ctestExitCode -ne 0 -and
                $ctestOutput.Contains('MARKER_BEFORE_FAILURE') -and
                $ctestOutput -match '(?m)0% tests passed'
            ) `
            -Message 'CTest must preserve a nonzero executable exit after earlier marker output.'
    }
    finally {
        $resolvedFixtureRoot = [IO.Path]::GetFullPath($fixtureRoot)
        Assert-True `
            -Condition ($resolvedFixtureRoot.StartsWith(
                $absoluteBuildDirectory + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase)) `
            -Message 'Refusing to clean an exit fixture outside the configured build directory.'
        if (Test-Path -LiteralPath $resolvedFixtureRoot) {
            Remove-Item -LiteralPath $resolvedFixtureRoot -Recurse -Force
        }
    }
}

function Assert-AutomationRequiredGate {
    param(
        [Parameter(Mandatory = $true)] [string]$AutomationWorkflowText,
        [Parameter(Mandatory = $true)] [string]$AutomationRunnerText
    )

    $triggerBody = Get-YamlTopLevelBody `
        -Text $AutomationWorkflowText `
        -Name 'on'
    foreach ($automaticTrigger in @('push', 'pull_request')) {
        Assert-True `
            -Condition ($triggerBody -match (
                '(?m)^  ' + [regex]::Escape($automaticTrigger) +
                ':\r?\n    branches:\r?\n      - master\r?$')) `
            -Message "Automation required gate must run automatically on $automaticTrigger for master."
    }

    $headlessJob = Get-YamlJobBody `
        -Text $AutomationWorkflowText `
        -JobId 'native-headless'
    Assert-True `
        -Condition (
            $headlessJob -notmatch '(?m)^    if:' -and
            $headlessJob -notmatch '(?m)^    continue-on-error:'
        ) `
        -Message 'The native/headless required job cannot be conditional or allowed to continue on error.'

    foreach ($buildStepContract in @(
        [pscustomobject]@{
            Name = 'Configure Ninja/MSVC Debug'
            Count = 1
            Preset = $null
            RequiredTargets = @()
            Configure = $true
        },
        [pscustomobject]@{
            Name = 'Build native automation targets'
            Count = 2
            Preset = $null
            RequiredTargets = @(
                'all',
                'specforge_asdf_labeling_production_benchmark'
            )
            Configure = $false
        },
        [pscustomobject]@{
            Name = 'Configure Ninja/MSVC static Release'
            Count = 1
            Preset = 'ninja-msvc-release-static'
            RequiredTargets = @()
            Configure = $true
        },
        [pscustomobject]@{
            Name = 'Build static Release native/headless targets'
            Count = 1
            Preset = 'ninja-msvc-release-static'
            RequiredTargets = @('all')
            Configure = $false
        }
    )) {
        $stepBody = Get-YamlStepBody `
            -Text $headlessJob `
            -StepName $buildStepContract.Name
        Assert-True `
            -Condition (
                $stepBody -notmatch '(?m)^        if:' -and
                $stepBody -notmatch '(?m)^        continue-on-error:'
            ) `
            -Message "$($buildStepContract.Name) cannot be conditional or allowed to continue on error."
        $stepAst = Get-PowerShellAst `
            -Text (Get-YamlStepRunScript `
                -Text $headlessJob `
                -StepName $buildStepContract.Name) `
            -Description $buildStepContract.Name
        $stepCommands = @(Get-TopLevelPowerShellCommands -Ast $stepAst)
        $wrapperCommands = @(
            $stepCommands | Where-Object {
                $elements = @(Get-CommandElementTexts -Command $_)
                $_.GetCommandName() -ceq 'powershell' -and
                (Get-CommandArgumentValue $elements '-File') -ceq
                    'scripts\build-ninja-msvc-debug.ps1'
            }
        )
        Assert-True `
            -Condition ($wrapperCommands.Count -eq $buildStepContract.Count) `
            -Message "$($buildStepContract.Name) must invoke every required build wrapper command."
        foreach ($wrapperCommand in $wrapperCommands) {
            $elements = @(Get-CommandElementTexts -Command $wrapperCommand)
            Assert-True `
                -Condition (
                    ([string](Get-CommandArgumentValue $elements '-VcvarsPath')).Trim(
                        '"', "'") -ceq '$env:SPECFORGE_VCVARS_PATH' -and
                    ($null -eq $buildStepContract.Preset -or
                        (Get-CommandArgumentValue $elements '-Preset') -ceq
                            $buildStepContract.Preset)
                ) `
                -Message "$($buildStepContract.Name) must use the resolved MSVC environment and required preset."
            Assert-CommandHasImmediateFailureGuard `
                -Ast $stepAst `
                -Command $wrapperCommand `
                -Description $buildStepContract.Name
        }
        if ($buildStepContract.Configure) {
            Assert-True `
                -Condition (@(
                    $wrapperCommands | Where-Object {
                        @(Get-CommandElementTexts -Command $_) -ccontains
                            '-Configure'
                    }
                ).Count -eq 1) `
                -Message "$($buildStepContract.Name) must perform configuration."
        }
        foreach ($target in $buildStepContract.RequiredTargets) {
            Assert-True `
                -Condition (@(
                    $wrapperCommands | Where-Object {
                        $elements = @(Get-CommandElementTexts -Command $_)
                        (Get-CommandArgumentValue $elements '-Target') -ceq
                            $target
                    }
                ).Count -eq 1) `
                -Message "$($buildStepContract.Name) must build $target exactly once."
        }
    }

    foreach ($gateContract in @(
        [pscustomobject]@{
            Name = 'Run required native/headless CTest gate'
            BuildDirectory = 'build\ninja-msvc-debug'
        },
        [pscustomobject]@{
            Name = 'Run required static Release CTest gate'
            BuildDirectory = 'build\ninja-msvc-release-static'
        }
    )) {
        $gateBody = Get-YamlStepBody `
            -Text $headlessJob `
            -StepName $gateContract.Name
        Assert-True `
            -Condition (
                $gateBody -notmatch '(?m)^        if:' -and
                $gateBody -notmatch '(?m)^        continue-on-error:'
            ) `
            -Message "$($gateContract.Name) cannot be conditional or allowed to continue on error."
        $gateAst = Get-PowerShellAst `
            -Text (Get-YamlStepRunScript `
                -Text $headlessJob `
                -StepName $gateContract.Name) `
            -Description $gateContract.Name
        $gateCommands = @(Get-TopLevelPowerShellCommands -Ast $gateAst)
        $runnerCommands = @(
            $gateCommands | Where-Object {
                $elements = @(Get-CommandElementTexts -Command $_)
                $_.GetCommandName() -ceq 'powershell' -and
                (Get-CommandArgumentValue $elements '-File') -ceq
                    'scripts\run-automation-ci.ps1'
            }
        )
        Assert-True `
            -Condition ($runnerCommands.Count -eq 1) `
            -Message "$($gateContract.Name) must actively invoke run-automation-ci.ps1 exactly once."
        $runnerElements = @(
            Get-CommandElementTexts -Command $runnerCommands[0]
        )
        Assert-True `
            -Condition (
                (Get-CommandArgumentValue $runnerElements '-Mode') -ceq
                    'Headless' -and
                (Get-CommandArgumentValue $runnerElements '-BuildDirectory') -ceq
                    $gateContract.BuildDirectory
            ) `
            -Message "$($gateContract.Name) must run the expected configured ci-headless graph."
        Assert-CommandHasImmediateFailureGuard `
            -Ast $gateAst `
            -Command $runnerCommands[0] `
            -Description $gateContract.Name
    }

    $runnerAst = Get-PowerShellAst `
        -Text $AutomationRunnerText `
        -Description 'Automation CTest runner'
    $labelAssignments = @(
        $runnerAst.FindAll(
            {
                param($node)
                $node -is
                    [Management.Automation.Language.AssignmentStatementAst] -and
                $node.Left.Extent.Text -ceq '$label'
            },
            $true)
    )
    Assert-True `
        -Condition (
            $labelAssignments.Count -eq 1 -and
            $labelAssignments[0].Right.Extent.Text -match
                '(?s)\$Mode\s+-eq\s+''Headless''.*''ci-headless''.*''real-gui'''
        ) `
        -Message 'Headless automation must resolve to the ci-headless CTest label.'
    $boundedCTestCommands = @(
        Get-PowerShellCommands -Ast $runnerAst | Where-Object {
            $_.GetCommandName() -ceq 'Invoke-BoundedCTest'
        }
    )
    Assert-True `
        -Condition ($boundedCTestCommands.Count -eq 1) `
        -Message 'Automation runner must invoke the bounded CTest gate exactly once.'
    $boundedElements = @(
        Get-CommandElementTexts -Command $boundedCTestCommands[0]
    )
    Assert-True `
        -Condition (
            (Get-CommandArgumentValue $boundedElements '-Label') -ceq '$label'
        ) `
        -Message 'The bounded CTest gate must receive the resolved ci-headless label.'
    $boundedFunction = @(
        $runnerAst.FindAll(
            {
                param($node)
                $node -is
                    [Management.Automation.Language.FunctionDefinitionAst] -and
                $node.Name -ceq 'Invoke-BoundedCTest'
            },
            $true)
    )
    Assert-True `
        -Condition (
            $boundedFunction.Count -eq 1 -and
            $boundedFunction[0].Body.Extent.Text -match
                "'--no-tests=error'" -and
            $boundedFunction[0].Body.Extent.Text -match
                '''-L'',\s*\$Label'
        ) `
        -Message 'The bounded CTest gate must fail on an empty selected label.'
}

function Assert-ReleaseWorkflowContract {
    param(
        [Parameter(Mandatory = $true)] [string]$WorkflowText,
        [Parameter(Mandatory = $true)] [string]$PresetsText
    )

    $presets = $PresetsText | ConvertFrom-Json
    $buildBody = Get-YamlJobBody -Text $WorkflowText -JobId 'build'
    $publishBody = Get-YamlJobBody -Text $WorkflowText -JobId 'publish'
    Assert-True `
        -Condition (
            $buildBody -notmatch '(?m)^    if:' -and
            $buildBody -notmatch '(?m)^    continue-on-error:'
        ) `
        -Message 'The required release build job cannot be conditional or allowed to continue on error.'
    Assert-True `
        -Condition ($buildBody -match '(?m)^    runs-on: windows-2022\r?$') `
        -Message 'The portable release build must use windows-2022.'
    Assert-True `
        -Condition ($publishBody -match
            '(?m)^    permissions:\r?\n      contents: write\r?$') `
        -Message 'The publish job must retain explicit release-write permission.'

    $basePreset = @(
        $presets.configurePresets |
            Where-Object { $_.name -ceq 'base' }
    )
    Assert-True `
        -Condition (
            $basePreset.Count -eq 1 -and
            $basePreset[0].cacheVariables.VCPKG_MANIFEST_FEATURES -ceq ''
        ) `
        -Message 'The shared preset must clear the retired manifest feature cache.'

    $staticReleaseBase = @(
        $presets.configurePresets |
            Where-Object { $_.name -ceq 'static-release-base' }
    )
    Assert-True `
        -Condition (
            $staticReleaseBase.Count -eq 1 -and
            $staticReleaseBase[0].cacheVariables.VCPKG_TARGET_TRIPLET -ceq
                'x64-windows-static'
        ) `
        -Message 'Static Release presets must use x64-windows-static.'
    $ninjaStaticRelease = @(
        $presets.configurePresets |
            Where-Object { $_.name -ceq 'ninja-msvc-release-static' }
    )
    Assert-True `
        -Condition (
            $ninjaStaticRelease.Count -eq 1 -and
            @($ninjaStaticRelease[0].inherits) -ccontains
                'static-release-base' -and
            $ninjaStaticRelease[0].cacheVariables.CMAKE_BUILD_TYPE -ceq
                'Release'
        ) `
        -Message 'The Ninja/MSVC static Release preset must remain the release verification path.'
    $visualStudioStaticRelease = @(
        $presets.configurePresets |
            Where-Object { $_.name -ceq 'vs2022-x64-release-static' }
    )
    Assert-True `
        -Condition (
            $visualStudioStaticRelease.Count -eq 1 -and
            @($visualStudioStaticRelease[0].inherits) -ccontains
                'static-release-base'
        ) `
        -Message 'Portable packaging must inherit the shared static Release triplet.'

    $toolchainScript = Get-YamlStepRunScript `
        -Text $buildBody `
        -StepName 'Select runner vcpkg'
    $toolchainAst = Get-PowerShellAst `
        -Text $toolchainScript `
        -Description 'Hosted-runner toolchain selection step'
    $toolchainAssignments = @($toolchainAst.EndBlock.Statements) |
        Where-Object {
            $_ -is [Management.Automation.Language.AssignmentStatementAst]
        }
    $vswhereAssignments = @(
        $toolchainAssignments |
            Where-Object { $_.Left.Extent.Text -ceq '$vswhere' }
    )
    $vcvarsAssignments = @(
        $toolchainAssignments |
            Where-Object { $_.Left.Extent.Text -ceq '$vcvars' }
    )
    Assert-True `
        -Condition (
            $vswhereAssignments.Count -eq 1 -and
            $vswhereAssignments[0].Right.Extent.Text.Contains(
                '${env:ProgramFiles(x86)}') -and
            $vswhereAssignments[0].Right.Extent.Text.Contains('vswhere.exe') -and
            $vcvarsAssignments.Count -eq 1 -and
            $vcvarsAssignments[0].Right.Extent.Text -match
                '(?s)-latest\s+-products\s+\*\s+-requires\s+Microsoft\.VisualStudio\.Component\.VC\.Tools\.x86\.x64\s+-find\s+''VC\\Auxiliary\\Build\\vcvars64\.bat'''
        ) `
        -Message 'Release workflow must dynamically resolve vcvars64.bat with hosted-runner vswhere.'
    $toolchainCommands = @(
        Get-TopLevelPowerShellCommands -Ast $toolchainAst
    )
    $vcvarsExports = @(
        $toolchainCommands | Where-Object {
            $elements = @(Get-CommandElementTexts -Command $_)
            $_.GetCommandName() -ceq 'Add-Content' -and
            (Get-CommandArgumentValue $elements '-LiteralPath') -ceq
                '$env:GITHUB_ENV' -and
            ([string](Get-CommandArgumentValue $elements '-Value')).Trim('"', "'") -ceq
                'SPECFORGE_VCVARS_PATH=$vcvars'
        }
    )
    Assert-True `
        -Condition ($vcvarsExports.Count -eq 1) `
        -Message 'Release workflow must export the dynamically resolved vcvars64.bat path.'

    $staticStepBody = Get-YamlStepBody `
        -Text $buildBody `
        -StepName 'Verify static Release FITS loading'
    Assert-True `
        -Condition (
            $staticStepBody -notmatch '(?m)^        if:' -and
            $staticStepBody -notmatch '(?m)^        continue-on-error:'
        ) `
        -Message 'Static Release FITS verification cannot be conditional or allowed to continue on error.'
    $staticScript = Get-YamlStepRunScript `
        -Text $buildBody `
        -StepName 'Verify static Release FITS loading'
    $staticAst = Get-PowerShellAst `
        -Text $staticScript `
        -Description 'Static Release FITS workflow step'
    $commands = @(Get-TopLevelPowerShellCommands -Ast $staticAst)
    $wrapperCommands = @(
        $commands | Where-Object {
            $elements = @(Get-CommandElementTexts -Command $_)
            $_.GetCommandName() -ceq 'powershell' -and
            (Get-CommandArgumentValue $elements '-File') -ceq
                'scripts\build-ninja-msvc-debug.ps1'
        }
    )
    Assert-True `
        -Condition ($wrapperCommands.Count -eq 3) `
        -Message 'Static Release FITS verification must actively invoke the build wrapper three times.'
    $wrapperElements = @(
        $wrapperCommands | ForEach-Object {
            ,@(Get-CommandElementTexts -Command $_)
        }
    )
    foreach ($elements in $wrapperElements) {
        Assert-True `
            -Condition (
                (Get-CommandArgumentValue $elements '-Preset') -ceq
                    'ninja-msvc-release-static' -and
                ([string](Get-CommandArgumentValue $elements '-VcvarsPath')).Trim(
                    '"', "'") -ceq '$env:SPECFORGE_VCVARS_PATH'
            ) `
            -Message 'Every FITS verification build command must use the static preset and the resolved hosted-runner vcvars path.'
    }
    Assert-True `
        -Condition (@(
            $wrapperElements | Where-Object { $_ -ccontains '-Configure' }
        ).Count -eq 1) `
        -Message 'Static Release FITS verification must configure exactly once.'
    foreach ($targetName in @(
        'specforge_fits_file_reader_tests',
        'specforge_loader_tests'
    )) {
        Assert-True `
            -Condition (@(
                $wrapperElements | Where-Object {
                    (Get-CommandArgumentValue $_ '-Target') -ceq $targetName
                }
            ).Count -eq 1) `
            -Message "Static Release workflow must build target $targetName exactly once."
    }

    $ctestCommands = @(
        $commands | Where-Object { $_.GetCommandName() -ceq 'ctest' }
    )
    Assert-True `
        -Condition ($ctestCommands.Count -eq 2) `
        -Message 'Static Release workflow must actively invoke exactly two CTest commands.'
    foreach ($testName in @(
        'specforge_fits_file_reader_tests',
        'specforge_loader_tests'
    )) {
        $expectedRegex = '^' + $testName + '$'
        $matchingCommands = @(
            $ctestCommands | Where-Object {
                $elements = @(Get-CommandElementTexts -Command $_)
                (Get-CommandArgumentValue $elements '-R') -ceq $expectedRegex
            }
        )
        Assert-True `
            -Condition ($matchingCommands.Count -eq 1) `
            -Message "Static Release workflow must run only the exact CTest name $testName."
        $elements = @(
            Get-CommandElementTexts -Command $matchingCommands[0]
        )
        Assert-True `
            -Condition (
                (Get-CommandArgumentValue $elements '--test-dir') -ceq
                    'build\ninja-msvc-release-static' -and
                $elements -ccontains '--output-on-failure' -and
                $elements -ccontains '--no-tests=error'
            ) `
            -Message "$testName must fail the Release workflow when its CTest registration is missing."
    }
    foreach ($requiredCommand in @($wrapperCommands) + @($ctestCommands)) {
        Assert-CommandHasImmediateFailureGuard `
            -Ast $staticAst `
            -Command $requiredCommand `
            -Description 'Static Release FITS verification command'
    }

    $packageScript = Get-YamlStepRunScript `
        -Text $buildBody `
        -StepName 'Build and verify portable package'
    $packageAst = Get-PowerShellAst `
        -Text $packageScript `
        -Description 'Portable build workflow step'
    $packageCommands = @(Get-PowerShellCommands -Ast $packageAst)
    Assert-True `
        -Condition (@(
            $packageCommands | Where-Object {
                $elements = @(Get-CommandElementTexts -Command $_)
                $_.GetCommandName() -ceq 'powershell' -and
                (Get-CommandArgumentValue $elements '-File') -ceq
                    'scripts\build-portable-from-head.ps1'
            }
        ).Count -eq 1) `
        -Message 'Release workflow must actively build the isolated HEAD Portable package.'

    $publishScript = Get-YamlStepRunScript `
        -Text $publishBody `
        -StepName 'Publish release'
    $publishAst = Get-PowerShellAst `
        -Text $publishScript `
        -Description 'Release publication workflow step'
    Assert-True `
        -Condition (@(
            Get-PowerShellCommands -Ast $publishAst | Where-Object {
                $elements = @(Get-CommandElementTexts -Command $_)
                $_.GetCommandName() -ceq 'gh' -and
                $elements.Count -ge 3 -and
                $elements[1] -ceq 'release' -and
                $elements[2] -ceq 'create'
            }
        ).Count -eq 1) `
        -Message 'Publish job must actively invoke gh release create.'

    $staticStepOffset = $buildBody.IndexOf(
        '- name: Verify static Release FITS loading',
        [StringComparison]::Ordinal)
    $uploadStepBody = Get-YamlStepBody `
        -Text $buildBody `
        -StepName 'Upload release artifacts'
    Assert-True `
        -Condition ($uploadStepBody -match
            '(?m)^        uses: actions/upload-artifact@v7\r?$') `
        -Message 'Release build must actively upload the verified artifacts.'
    $uploadStepOffset = $buildBody.IndexOf(
        '- name: Upload release artifacts',
        [StringComparison]::Ordinal)
    Assert-True `
        -Condition (
            $staticStepOffset -ge 0 -and
            $uploadStepOffset -gt $staticStepOffset
        ) `
        -Message 'Both static Release FITS tests must finish before upload.'
}

function Invoke-SelectedContractValidation {
    param(
        [string]$WorkflowText = '',
        [string]$PresetsText = '',
        [string]$DebugCTestJson = '',
        [string]$DebugBuildGraphText = '',
        [string]$DebugCacheText = '',
        [string]$DebugBuildDirectory = '',
        [string]$ReleaseCTestJson = '',
        [string]$ReleaseBuildGraphText = '',
        [string]$ReleaseCacheText = '',
        [string]$ReleaseBuildDirectory = '',
        [string]$LoaderText = '',
        [string]$ReaderText = '',
        [string]$AutomationWorkflowText = '',
        [string]$AutomationRunnerText = ''
    )

    if ($ContractArea -ceq 'WorkflowStructure') {
        Assert-ReleaseWorkflowContract `
            -WorkflowText $WorkflowText `
            -PresetsText $PresetsText
        Assert-AutomationRequiredGate `
            -AutomationWorkflowText $AutomationWorkflowText `
            -AutomationRunnerText $AutomationRunnerText
        return
    }

    Assert-CMakeAndFitsTestContract `
        -CTestJson $DebugCTestJson `
        -BuildGraphText $DebugBuildGraphText `
        -CacheText $DebugCacheText `
        -BuildDirectory $DebugBuildDirectory `
        -Configuration 'Debug' `
        -Triplet 'x64-windows' `
        -RuntimeFlag '-MDd' `
        -LoaderText $LoaderText `
        -ReaderText $ReaderText
    Assert-CMakeAndFitsTestContract `
        -CTestJson $ReleaseCTestJson `
        -BuildGraphText $ReleaseBuildGraphText `
        -CacheText $ReleaseCacheText `
        -BuildDirectory $ReleaseBuildDirectory `
        -Configuration 'Release' `
        -Triplet 'x64-windows-static' `
        -RuntimeFlag '-MT' `
        -LoaderText $LoaderText `
        -ReaderText $ReaderText
}

$paths = if ($ContractArea -ceq 'WorkflowStructure') {
    [ordered]@{
        WorkflowText = '.github\workflows\release.yml'
        PresetsText = 'CMakePresets.json'
        AutomationWorkflowText = '.github\workflows\automation.yml'
        AutomationRunnerText = 'scripts\run-automation-ci.ps1'
    }
}
else {
    [ordered]@{
        LoaderText = 'tests\spectrum_loader_tests.cpp'
        ReaderText = 'tests\fits_file_reader_tests.cpp'
    }
}
$contractInputs = @{}
foreach ($entry in $paths.GetEnumerator()) {
    $path = Join-Path $RepoRoot $entry.Value
    Assert-True `
        -Condition (Test-Path -LiteralPath $path -PathType Leaf) `
        -Message "Required release contract source is missing: $path"
    $contractInputs[$entry.Key] = Get-Content -Raw -LiteralPath $path
}

if ($ContractArea -ceq 'ConfiguredBuild') {
    foreach ($configuredBuild in @(
    [pscustomobject]@{
        Prefix = 'Debug'
        Directory = $DebugBuildDirectory
    },
    [pscustomobject]@{
        Prefix = 'Release'
        Directory = $ReleaseBuildDirectory
    }
    )) {
        $absoluteDirectory = [IO.Path]::GetFullPath($configuredBuild.Directory)
        $buildGraphPath = Join-Path $absoluteDirectory 'build.ninja'
        $cachePath = Join-Path $absoluteDirectory 'CMakeCache.txt'
        foreach ($configuredPath in @($buildGraphPath, $cachePath)) {
            Assert-True `
                -Condition (Test-Path -LiteralPath $configuredPath -PathType Leaf) `
                -Message "Configured build fact is missing: $configuredPath"
        }
        $ctestOutput = @(
            & ctest --test-dir $absoluteDirectory --show-only=json-v1
        ) -join [Environment]::NewLine
        Assert-True `
            -Condition ($LASTEXITCODE -eq 0) `
            -Message "Could not read the $($configuredBuild.Prefix) CTest graph."
        $contractInputs["$($configuredBuild.Prefix)CTestJson"] = $ctestOutput
        $contractInputs["$($configuredBuild.Prefix)BuildGraphText"] =
            Get-Content -Raw -LiteralPath $buildGraphPath
        $contractInputs["$($configuredBuild.Prefix)CacheText"] =
            Get-Content -Raw -LiteralPath $cachePath
        $contractInputs["$($configuredBuild.Prefix)BuildDirectory"] =
            $absoluteDirectory
    }
}

Invoke-SelectedContractValidation @contractInputs

function New-TextMutationCase {
    param(
        [Parameter(Mandatory = $true)] [string]$Description,
        [Parameter(Mandatory = $true)] [string]$ExpectedId,
        [Parameter(Mandatory = $true)] [string]$Key,
        [Parameter(Mandatory = $true)] [scriptblock]$Transform
    )

    return [pscustomobject]@{
        Description = $Description
        ExpectedId = $ExpectedId
        Key = $Key
        Transform = $Transform
    }
}

$baselineValidation = {
    Invoke-SelectedContractValidation @contractInputs
}
$textMutationValidator = {
    param($case)

    $mutatedInputs = $contractInputs.Clone()
    $original = [string]$contractInputs[$case.Key]
    $mutated = [string](& $case.Transform $original)
    Assert-ContractRule `
        -Condition ($mutated -cne $original) `
        -Id 'SF-HARNESS-MUTATION' `
        -Message "$($case.Description) mutation setup did not change $($case.Key)."
    $mutatedInputs[$case.Key] = $mutated
    Invoke-SelectedContractValidation @mutatedInputs
}
$newline = "`n"
foreach ($text in $contractInputs.Values) {
    if ($text -is [string] -and $text.Contains("`r`n")) {
        $newline = "`r`n"
        break
    }
}

if ($ContractArea -ceq 'WorkflowStructure') {
    $workflowMutations = @(
        New-TextMutationCase `
            -Description 'Commented Release CTest commands' `
            -ExpectedId 'SF-WF-STATIC-SCRIPT-SYNTAX' `
            -Key 'WorkflowText' `
            -Transform {
                param($text)
                [regex]::Replace(
                    $text,
                    '(?m)^          ctest --test-dir build\\ninja-msvc-release-static `\r?$',
                    '          # ctest --test-dir build\ninja-msvc-release-static `')
            };
        New-TextMutationCase `
            -Description 'Release build job hidden behind job-level if(false)' `
            -ExpectedId 'SF-WF-RELEASE-JOB-POLICY' `
            -Key 'WorkflowText' `
            -Transform {
                param($text)
                $text.Replace(
                    '  build:',
                    '  build:' + $newline + '    if: ${{ false }}')
            };
        New-TextMutationCase `
            -Description 'Release build job allowed to continue on error' `
            -ExpectedId 'SF-WF-RELEASE-JOB-POLICY' `
            -Key 'WorkflowText' `
            -Transform {
                param($text)
                $text.Replace(
                    '  build:',
                    '  build:' + $newline + '    continue-on-error: true')
            };
        New-TextMutationCase `
            -Description 'Native/headless required job hidden behind job-level if(false)' `
            -ExpectedId 'SF-WF-AUTOMATION-JOB-POLICY' `
            -Key 'AutomationWorkflowText' `
            -Transform {
                param($text)
                $text.Replace(
                    '  native-headless:',
                    '  native-headless:' + $newline + '    if: ${{ false }}')
            };
        New-TextMutationCase `
            -Description 'Automatic gate missing static Release configuration' `
            -ExpectedId 'SF-WF-AUTOMATION-RELEASE-CONFIG-STEP' `
            -Key 'AutomationWorkflowText' `
            -Transform {
                param($text)
                [regex]::Replace(
                    $text,
                    '(?ms)^      - name: Configure Ninja/MSVC static Release\r?\n.*?(?=^      - name: )',
                    '')
            };
        New-TextMutationCase `
            -Description 'Automatic static Release gate rerouted to Debug graph' `
            -ExpectedId 'SF-WF-AUTOMATION-RELEASE-GATE' `
            -Key 'AutomationWorkflowText' `
            -Transform {
                param($text)
                $text.Replace(
                    '            -BuildDirectory build\ninja-msvc-release-static `',
                    '            -BuildDirectory build\ninja-msvc-debug `')
            };
        New-TextMutationCase `
            -Description 'Debug build failure hidden behind nested if(false)' `
            -ExpectedId 'SF-WF-AUTOMATION-DEBUG-EXIT' `
            -Key 'AutomationWorkflowText' `
            -Transform {
                param($text)
                $nestedGuard = @(
                    '          if ($LASTEXITCODE -ne 0) {',
                    '            if ($false) {',
                    '              throw "Debug all-target build failed with exit code $LASTEXITCODE."',
                    '            }',
                    '          }'
                ) -join $newline
                [regex]::Replace(
                    $text,
                    '(?ms)^          if \(\$LASTEXITCODE -ne 0\) \{\r?\n' +
                        '            throw "Debug all-target build failed with exit code ' +
                        '\$LASTEXITCODE\."\r?\n          \}',
                    $nestedGuard,
                    1)
            };
        New-TextMutationCase `
            -Description 'Here-string Release CTest decoy' `
            -ExpectedId 'SF-WF-STATIC-CTEST-COUNT' `
            -Key 'WorkflowText' `
            -Transform {
                param($text)
                $decoy = @(
                    "          `$decoy = @'",
                    '          ctest --test-dir build\ninja-msvc-release-static --output-on-failure --no-tests=error -R ''^specforge_fits_file_reader_tests$''',
                    "          '@"
                ) -join $newline
                $pattern =
                    '(?ms)^          ctest --test-dir build\\ninja-msvc-release-static `\r?\n' +
                        '            --output-on-failure `\r?\n' +
                        '            --no-tests=error `\r?\n' +
                        '            -R ''\^specforge_fits_file_reader_tests\$'''
                $evaluator = [Text.RegularExpressions.MatchEvaluator]{
                    param($match)
                    return $decoy
                }
                [regex]::new($pattern).Replace($text, $evaluator, 1)
            };
        New-TextMutationCase `
            -Description 'Release commands hidden in if(false)' `
            -ExpectedId 'SF-WF-STATIC-WRAPPER-COUNT' `
            -Key 'WorkflowText' `
            -Transform {
                param($text)
                $stepBody = Get-YamlStepBody `
                    -Text $text `
                    -StepName 'Verify static Release FITS loading'
                $header = [regex]::Match($stepBody, '(?m)^        run: \|\r?\n')
                $script = Get-YamlStepRunScript `
                    -Text $text `
                    -StepName 'Verify static Release FITS loading'
                $lines = @('          if ($false) {')
                $lines += @([regex]::Split($script, '\r?\n') |
                    ForEach-Object { '            ' + $_ })
                $lines += '          }'
                $replacement = $stepBody.Substring(
                    0,
                    $header.Index + $header.Length) +
                    ($lines -join $newline) + $newline
                $text.Replace($stepBody, $replacement)
            };
        New-TextMutationCase `
            -Description 'Conditional static Release workflow step' `
            -ExpectedId 'SF-WF-STATIC-STEP-POLICY' `
            -Key 'WorkflowText' `
            -Transform {
                param($text)
                $line = '      - name: Verify static Release FITS loading'
                $text.Replace($line, $line + $newline + '        if: ${{ false }}')
            };
        New-TextMutationCase `
            -Description 'Continue-on-error static Release workflow step' `
            -ExpectedId 'SF-WF-STATIC-STEP-POLICY' `
            -Key 'WorkflowText' `
            -Transform {
                param($text)
                $line = '      - name: Verify static Release FITS loading'
                $text.Replace($line, $line + $newline + '        continue-on-error: true')
            };
        New-TextMutationCase `
            -Description 'Static Release command without a real exit-code guard' `
            -ExpectedId 'SF-WF-STATIC-EXIT-GUARD' `
            -Key 'WorkflowText' `
            -Transform {
                param($text)
                $stepBody = Get-YamlStepBody `
                    -Text $text `
                    -StepName 'Verify static Release FITS loading'
                $text.Replace(
                    $stepBody,
                    $stepBody.Replace('if ($LASTEXITCODE -ne 0)', 'if ($false)'))
            };
        New-TextMutationCase `
            -Description 'Static Release wrapper commands without hosted-runner vcvars' `
            -ExpectedId 'SF-WF-STATIC-WRAPPER-ARGS' `
            -Key 'WorkflowText' `
            -Transform {
                param($text)
                [regex]::Replace(
                    $text,
                    '(?m)^            -VcvarsPath "\$env:SPECFORGE_VCVARS_PATH" `\r?\n',
                    '')
            };
        New-TextMutationCase `
            -Description 'Required automation gate limited to workflow_dispatch' `
            -ExpectedId 'SF-WF-PUSH-TRIGGER' `
            -Key 'AutomationWorkflowText' `
            -Transform {
                param($text)
                [regex]::Replace(
                    $text,
                    '(?ms)^  push:\r?\n    branches:\r?\n      - master\r?\n' +
                        '  pull_request:\r?\n    branches:\r?\n      - master\r?\n',
                    '')
            };
    )

    Invoke-ContractMutationTable `
        -Cases $workflowMutations `
        -ValidateBaseline $baselineValidation `
        -ValidateCase $textMutationValidator
    Write-Host 'Release workflow structure and mutation contracts passed.'
    return
}

Assert-CtestExitCodeRemainsAuthoritative `
    -BuildDirectory $contractInputs.DebugBuildDirectory

$configuredMutations = @(
    [pscustomobject]@{
        Description = 'CTest marker overrides a later nonzero exit'
        ExpectedId = 'SF-CFG-LOADER-EXIT-AUTHORITY'
        Validate = {
            $mutatedInputs = $contractInputs.Clone()
            $model = $contractInputs.DebugCTestJson | ConvertFrom-Json
            $tests = @($model.tests | Where-Object {
                $_.name -ceq 'specforge_loader_tests'
            })
            Assert-ContractRule `
                -Condition ($tests.Count -eq 1) `
                -Id 'SF-HARNESS-MUTATION' `
                -Message 'PASS_REGULAR_EXPRESSION mutation could not find loader test.'
            $tests[0].properties = @(
                @($tests[0].properties) +
                    [pscustomobject]@{
                        name = 'PASS_REGULAR_EXPRESSION'
                        value = @('MARKER_BEFORE_FAILURE')
                    })
            $mutatedInputs.DebugCTestJson = $model | ConvertTo-Json -Depth 20
            Invoke-SelectedContractValidation @mutatedInputs
        }
    };
    [pscustomobject]@{
        Description = 'Static Release loader removed from the automatic required gate'
        ExpectedId = 'SF-CFG-LOADER-GATE'
        Validate = {
            $mutatedInputs = $contractInputs.Clone()
            $model = $contractInputs.ReleaseCTestJson | ConvertFrom-Json
            $tests = @($model.tests | Where-Object {
                $_.name -ceq 'specforge_loader_tests'
            })
            $labels = @($tests[0].properties | Where-Object {
                $_.name -ceq 'LABELS'
            })
            Assert-ContractRule `
                -Condition ($tests.Count -eq 1 -and $labels.Count -eq 1) `
                -Id 'SF-HARNESS-MUTATION' `
                -Message 'Loader gate mutation could not find configured labels.'
            $labels[0].value = @($labels[0].value | Where-Object {
                $_ -cne 'ci-headless'
            })
            $mutatedInputs.ReleaseCTestJson =
                $model | ConvertTo-Json -Depth 20
            Invoke-SelectedContractValidation @mutatedInputs
        }
    };
    New-TextMutationCase `
        -Description 'Static Release configured with dynamic triplet' `
        -ExpectedId 'SF-CFG-RELEASE-TRIPLET' `
        -Key 'ReleaseCacheText' `
        -Transform {
            param($text)
            $text.Replace(
                'VCPKG_TARGET_TRIPLET:STRING=x64-windows-static',
                'VCPKG_TARGET_TRIPLET:STRING=x64-windows')
        };
    New-TextMutationCase `
        -Description 'Static Release graph compiled with dynamic CRT' `
        -ExpectedId 'SF-CFG-RELEASE-CRT' `
        -Key 'ReleaseBuildGraphText' `
        -Transform {
            param($text)
            $text.Replace(' -MT ', ' -MD ')
        };
    New-TextMutationCase `
        -Description 'Missing static Release FITS executable graph' `
        -ExpectedId 'SF-CFG-LOADER-GRAPH' `
        -Key 'ReleaseBuildGraphText' `
        -Transform {
            param($text)
            [regex]::Replace(
                $text,
                '(?m)^build specforge_loader_tests\.exe:[^\r\n]*\r?\n',
                '')
        };
    New-TextMutationCase `
        -Description 'Deleted loader gzip test invocation' `
        -ExpectedId 'SF-CFG-LOADER-GZIP-COVERAGE' `
        -Key 'LoaderText' `
        -Transform {
            param($text)
            $text.Replace(
                '    TestLoadsGzippedFitsSpectrum();',
                '    // TestLoadsGzippedFitsSpectrum();')
        };
    New-TextMutationCase `
        -Description 'Loader gzip invocation hidden behind #if 0' `
        -ExpectedId 'SF-CFG-LOADER-GZIP-COVERAGE' `
        -Key 'LoaderText' `
        -Transform {
            param($text)
            $text.Replace(
                '    TestLoadsGzippedFitsSpectrum();',
                '#if 0' + $newline +
                    '    TestLoadsGzippedFitsSpectrum();' + $newline +
                    '#endif')
        };
    New-TextMutationCase `
        -Description 'Loader gzip invocation hidden behind if(false)' `
        -ExpectedId 'SF-CFG-LOADER-GZIP-COVERAGE' `
        -Key 'LoaderText' `
        -Transform {
            param($text)
            $text.Replace(
                '    TestLoadsGzippedFitsSpectrum();',
                '    if (false) { TestLoadsGzippedFitsSpectrum(); }')
        };
    [pscustomobject]@{
        Description = 'Release artifact contract removed from ci-headless required gate'
        ExpectedId = 'SF-CFG-ARTIFACT-GATE'
        Validate = {
            $mutatedInputs = $contractInputs.Clone()
            $model = $contractInputs.DebugCTestJson | ConvertFrom-Json
            $tests = @($model.tests | Where-Object {
                $_.name -ceq 'specforge_release_artifacts_tests'
            })
            $labels = @($tests[0].properties | Where-Object {
                $_.name -ceq 'LABELS'
            })
            Assert-ContractRule `
                -Condition ($tests.Count -eq 1 -and $labels.Count -eq 1) `
                -Id 'SF-HARNESS-MUTATION' `
                -Message 'Artifact gate mutation could not find configured labels.'
            $labels[0].value = @($labels[0].value | Where-Object {
                $_ -cne 'ci-headless'
            })
            $mutatedInputs.DebugCTestJson =
                $model | ConvertTo-Json -Depth 20
            Invoke-SelectedContractValidation @mutatedInputs
        }
    };
)

Invoke-ContractMutationTable `
    -Cases $configuredMutations `
    -ValidateBaseline $baselineValidation `
    -ValidateCase $textMutationValidator

$cmakeFixtureMutations = @(
    [pscustomobject]@{
        Description = 'Bracket-commented executable and CTest decoy'
        Body = @'
#[=[
add_executable(specforge_loader_tests tests/spectrum_loader_tests.cpp)
target_link_libraries(specforge_loader_tests PRIVATE specforge_core CFITSIO::cfitsio)
add_test(NAME specforge_loader_tests COMMAND specforge_loader_tests)
]=]
add_custom_target(specforge_loader_tests)
'@
    }
    [pscustomobject]@{
        Description = 'Bracket-argument executable and CTest decoy'
        Body = @'
set(decoy [==[
add_executable(specforge_loader_tests tests/spectrum_loader_tests.cpp)
target_link_libraries(specforge_loader_tests PRIVATE specforge_core CFITSIO::cfitsio)
add_test(NAME specforge_loader_tests COMMAND specforge_loader_tests)
]==])
add_custom_target(specforge_loader_tests)
'@
    }
    [pscustomobject]@{
        Description = 'if(FALSE) executable and CTest decoy'
        Body = @'
if(FALSE)
    add_executable(specforge_loader_tests tests/spectrum_loader_tests.cpp)
    target_link_libraries(specforge_loader_tests PRIVATE specforge_core CFITSIO::cfitsio)
    add_test(NAME specforge_loader_tests COMMAND specforge_loader_tests)
endif()
add_custom_target(specforge_loader_tests)
'@
    }
)
foreach ($case in $cmakeFixtureMutations) {
    Assert-CMakeMutationCannotSatisfyConfiguredGraph `
        -Description $case.Description `
        -BuildDirectory $contractInputs.DebugBuildDirectory `
        -Body $case.Body
}

Write-Host 'Configured build facts, production FITS chain, and mutation contracts passed.'
