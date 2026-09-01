Set-StrictMode -Version 3.0

function Set-PeFixtureUInt16 {
    param(
        [Parameter(Mandatory = $true)] [byte[]]$Bytes,
        [Parameter(Mandatory = $true)] [int]$Offset,
        [Parameter(Mandatory = $true)] [uint16]$Value
    )

    [Array]::Copy([BitConverter]::GetBytes($Value), 0, $Bytes, $Offset, 2)
}

function Set-PeFixtureUInt32 {
    param(
        [Parameter(Mandatory = $true)] [byte[]]$Bytes,
        [Parameter(Mandatory = $true)] [int]$Offset,
        [Parameter(Mandatory = $true)] [uint32]$Value
    )

    [Array]::Copy([BitConverter]::GetBytes($Value), 0, $Bytes, $Offset, 4)
}

function Set-PeFixtureUInt64 {
    param(
        [Parameter(Mandatory = $true)] [byte[]]$Bytes,
        [Parameter(Mandatory = $true)] [int]$Offset,
        [Parameter(Mandatory = $true)] [uint64]$Value
    )

    [Array]::Copy([BitConverter]::GetBytes($Value), 0, $Bytes, $Offset, 8)
}

function Set-PeFixtureAsciiString {
    param(
        [Parameter(Mandatory = $true)] [byte[]]$Bytes,
        [Parameter(Mandatory = $true)] [int]$Offset,
        [Parameter(Mandatory = $true)] [string]$Value
    )

    $encoded = [Text.Encoding]::ASCII.GetBytes($Value + [char]0)
    [Array]::Copy($encoded, 0, $Bytes, $Offset, $encoded.Length)
}

function New-PeImportFixture {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [AllowEmptyString()]
        [string]$RegularImport,
        [Parameter(Mandatory = $true)] [AllowEmptyString()]
        [string]$DelayImport
    )

    $bytes = [byte[]]::new(0x400)
    $bytes[0] = [byte][char]'M'
    $bytes[1] = [byte][char]'Z'
    Set-PeFixtureUInt32 $bytes 0x3c 0x80

    $peOffset = 0x80
    Set-PeFixtureUInt32 $bytes $peOffset 0x00004550
    Set-PeFixtureUInt16 $bytes ($peOffset + 4) 0x8664
    Set-PeFixtureUInt16 $bytes ($peOffset + 6) 1
    Set-PeFixtureUInt16 $bytes ($peOffset + 20) 0x00f0

    $optionalHeaderOffset = $peOffset + 24
    Set-PeFixtureUInt16 $bytes $optionalHeaderOffset 0x020b
    Set-PeFixtureUInt64 $bytes ($optionalHeaderOffset + 24) 0x0000000140000000
    Set-PeFixtureUInt32 $bytes ($optionalHeaderOffset + 60) 0x200
    Set-PeFixtureUInt32 $bytes ($optionalHeaderOffset + 108) 16

    $dataDirectoryOffset = $optionalHeaderOffset + 112
    if (-not [string]::IsNullOrEmpty($RegularImport)) {
        Set-PeFixtureUInt32 $bytes ($dataDirectoryOffset + 8) 0x1000
        Set-PeFixtureUInt32 $bytes ($dataDirectoryOffset + 12) 40
    }
    if (-not [string]::IsNullOrEmpty($DelayImport)) {
        Set-PeFixtureUInt32 $bytes ($dataDirectoryOffset + 104) 0x1040
        Set-PeFixtureUInt32 $bytes ($dataDirectoryOffset + 108) 64
    }

    $sectionOffset = $optionalHeaderOffset + 0x00f0
    Set-PeFixtureAsciiString $bytes $sectionOffset '.rdata'
    Set-PeFixtureUInt32 $bytes ($sectionOffset + 8) 0x200
    Set-PeFixtureUInt32 $bytes ($sectionOffset + 12) 0x1000
    Set-PeFixtureUInt32 $bytes ($sectionOffset + 16) 0x200
    Set-PeFixtureUInt32 $bytes ($sectionOffset + 20) 0x200

    if (-not [string]::IsNullOrEmpty($RegularImport)) {
        Set-PeFixtureUInt32 $bytes 0x20c 0x10c0
        Set-PeFixtureAsciiString $bytes 0x2c0 $RegularImport
    }
    if (-not [string]::IsNullOrEmpty($DelayImport)) {
        Set-PeFixtureUInt32 $bytes 0x240 1
        Set-PeFixtureUInt32 $bytes 0x244 0x10e0
        Set-PeFixtureAsciiString $bytes 0x2e0 $DelayImport
    }

    [IO.File]::WriteAllBytes($Path, $bytes)
}
