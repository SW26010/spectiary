Set-StrictMode -Version 3.0

function New-ContractViolation {
    param(
        [Parameter(Mandatory = $true)] [string]$Id,
        [Parameter(Mandatory = $true)] [string]$Message
    )

    return [InvalidOperationException]::new("[$Id] $Message")
}

function Assert-ContractRule {
    param(
        [Parameter(Mandatory = $true)] [bool]$Condition,
        [Parameter(Mandatory = $true)] [string]$Id,
        [Parameter(Mandatory = $true)] [string]$Message
    )

    if (-not $Condition) {
        throw (New-ContractViolation -Id $Id -Message $Message)
    }
}

function Get-ContractViolationId {
    param([Parameter(Mandatory = $true)] [Management.Automation.ErrorRecord]$ErrorRecord)

    $match = [regex]::Match(
        $ErrorRecord.Exception.Message,
        '^\[(?<id>SF-[A-Z0-9-]+)\]')
    if (-not $match.Success) {
        return $null
    }
    return $match.Groups['id'].Value
}

function Assert-ContractMutation {
    param(
        [Parameter(Mandatory = $true)] [string]$Description,
        [Parameter(Mandatory = $true)] [string]$ExpectedId,
        [Parameter(Mandatory = $true)] [scriptblock]$ValidateBaseline,
        [Parameter(Mandatory = $true)] [scriptblock]$ValidateMutation
    )

    try {
        & $ValidateBaseline
    }
    catch {
        $baselineId = Get-ContractViolationId -ErrorRecord $_
        $suffix = if ($null -eq $baselineId) {
            $_.Exception.Message
        }
        else {
            "$baselineId`: $($_.Exception.Message)"
        }
        throw (New-ContractViolation `
            -Id 'SF-HARNESS-BASELINE' `
            -Message "$Description baseline failed: $suffix")
    }

    $failure = $null
    try {
        & $ValidateMutation
    }
    catch {
        $failure = $_
    }
    Assert-ContractRule `
        -Condition ($null -ne $failure) `
        -Id 'SF-HARNESS-NOT-REJECTED' `
        -Message "$Description mutation unexpectedly satisfied the contract."

    $actualId = Get-ContractViolationId -ErrorRecord $failure
    Assert-ContractRule `
        -Condition ($actualId -ceq $ExpectedId) `
        -Id 'SF-HARNESS-WRONG-ID' `
        -Message (
            "$Description expected [$ExpectedId] but received " +
            $(if ($null -eq $actualId) {
                "an unclassified error: $($failure.Exception.Message)"
            }
            else {
                "[$actualId]: $($failure.Exception.Message)"
            }))
}

function Invoke-ContractMutationTable {
    param(
        [Parameter(Mandatory = $true)] [object[]]$Cases,
        [Parameter(Mandatory = $true)] [scriptblock]$ValidateBaseline,
        [Parameter(Mandatory = $false)] [scriptblock]$ValidateCase
    )

    foreach ($case in $Cases) {
        Assert-ContractRule `
            -Condition (-not [string]::IsNullOrWhiteSpace($case.Description)) `
            -Id 'SF-HARNESS-CASE' `
            -Message 'Mutation table contains a case without a description.'
        Assert-ContractRule `
            -Condition (-not [string]::IsNullOrWhiteSpace($case.ExpectedId)) `
            -Id 'SF-HARNESS-CASE' `
            -Message "$($case.Description) has no expected contract ID."
        $caseValidator = $case.PSObject.Properties['Validate']
        Assert-ContractRule `
            -Condition (
                ($null -ne $caseValidator -and
                 $caseValidator.Value -is [scriptblock]) -or
                $ValidateCase -is [scriptblock]) `
            -Id 'SF-HARNESS-CASE' `
            -Message "$($case.Description) has no mutation validator."
        $validateMutation = if ($null -ne $caseValidator -and
            $caseValidator.Value -is [scriptblock]) {
            $caseValidator.Value
        }
        else {
            { & $ValidateCase $case }
        }
        Assert-ContractMutation `
            -Description $case.Description `
            -ExpectedId $case.ExpectedId `
            -ValidateBaseline $ValidateBaseline `
            -ValidateMutation $validateMutation
    }
}
