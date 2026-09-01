Set-StrictMode -Version 3.0

function Assert-PeRange {
    param(
        [Parameter(Mandatory = $true)] [byte[]]$Bytes,
        [Parameter(Mandatory = $true)] [int]$Offset,
        [Parameter(Mandatory = $true)] [int]$Count,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    if ($Offset -lt 0 -or $Count -lt 0 -or
        $Offset -gt $Bytes.Length - $Count) {
        throw "[SF-PE-PARSE-RANGE] $Description is outside the PE file."
    }
}

function Convert-PeRvaToFileOffset {
    param(
        [Parameter(Mandatory = $true)] [uint32]$Rva,
        [Parameter(Mandatory = $true)] [object[]]$Sections,
        [Parameter(Mandatory = $true)] [uint32]$SizeOfHeaders,
        [Parameter(Mandatory = $true)] [int]$FileLength
    )

    if ($Rva -lt $SizeOfHeaders) {
        return [int]$Rva
    }
    foreach ($section in $Sections) {
        $sectionStart = [uint64]$section.VirtualAddress
        $sectionSize = [Math]::Max(
            [uint64]$section.VirtualSize,
            [uint64]$section.RawSize)
        $rvaValue = [uint64]$Rva
        if ($rvaValue -ge $sectionStart -and
            $rvaValue -lt $sectionStart + $sectionSize) {
            $fileOffset =
                [uint64]$section.RawOffset + ($rvaValue - $sectionStart)
            if ($fileOffset -ge [uint64]$FileLength) {
                throw "[SF-PE-PARSE-RVA] PE RVA 0x$($Rva.ToString('x')) resolves outside the file."
            }
            return [int]$fileOffset
        }
    }
    throw "[SF-PE-PARSE-RVA] PE RVA 0x$($Rva.ToString('x')) does not map to a section."
}

function Get-PeAsciiString {
    param(
        [Parameter(Mandatory = $true)] [byte[]]$Bytes,
        [Parameter(Mandatory = $true)] [int]$Offset,
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    Assert-PeRange $Bytes $Offset 1 "$Description in '$Path'"
    $end = $Offset
    while ($end -lt $Bytes.Length -and $Bytes[$end] -ne 0) {
        ++$end
    }
    if ($end -eq $Bytes.Length) {
        throw "[SF-PE-PARSE-STRING] $Description is not null-terminated in '$Path'."
    }
    if ($end -eq $Offset) {
        throw "[SF-PE-PARSE-STRING] $Description is empty in '$Path'."
    }
    return [Text.Encoding]::ASCII.GetString(
        $Bytes,
        $Offset,
        $end - $Offset)
}

function Get-PeImportedDllNames {
    param([Parameter(Mandatory = $true)] [string]$Path)

    $bytes = [IO.File]::ReadAllBytes($Path)
    Assert-PeRange $bytes 0x3c 4 "PE DOS header in '$Path'"
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
    Assert-PeRange $bytes $peOffset 24 "PE signature and COFF header in '$Path'"
    if ([BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550) {
        throw "[SF-PE-PARSE-SIGNATURE] Executable does not contain a valid PE signature: $Path"
    }

    $sectionCount = [BitConverter]::ToUInt16($bytes, $peOffset + 6)
    $optionalHeaderSize = [BitConverter]::ToUInt16($bytes, $peOffset + 20)
    $optionalHeaderOffset = $peOffset + 24
    Assert-PeRange `
        $bytes `
        $optionalHeaderOffset `
        $optionalHeaderSize `
        "PE optional header in '$Path'"
    $optionalMagic = [BitConverter]::ToUInt16(
        $bytes,
        $optionalHeaderOffset)
    $optionalLayout = switch ($optionalMagic) {
        0x010b {
            [pscustomobject]@{
                DataDirectoryOffset = $optionalHeaderOffset + 96
                DirectoryCountOffset = $optionalHeaderOffset + 92
                ImageBase = [uint64][BitConverter]::ToUInt32(
                    $bytes,
                    $optionalHeaderOffset + 28)
            }
        }
        0x020b {
            [pscustomobject]@{
                DataDirectoryOffset = $optionalHeaderOffset + 112
                DirectoryCountOffset = $optionalHeaderOffset + 108
                ImageBase = [BitConverter]::ToUInt64(
                    $bytes,
                    $optionalHeaderOffset + 24)
            }
        }
        default {
            throw "[SF-PE-PARSE-MAGIC] Executable has unsupported PE optional-header magic 0x$($optionalMagic.ToString('x')): $Path"
        }
    }
    $dataDirectoryOffset = $optionalLayout.DataDirectoryOffset
    $sizeOfHeaders = [BitConverter]::ToUInt32(
        $bytes,
        $optionalHeaderOffset + 60)
    Assert-PeRange `
        $bytes `
        $optionalLayout.DirectoryCountOffset `
        4 `
        "PE data-directory count in '$Path'"
    $dataDirectoryCount = [BitConverter]::ToUInt32(
        $bytes,
        $optionalLayout.DirectoryCountOffset)
    $requiredDirectoryCount = [Math]::Min(
        [uint64]$dataDirectoryCount,
        [uint64]14)
    Assert-PeRange `
        $bytes `
        $dataDirectoryOffset `
        ([int]($requiredDirectoryCount * 8)) `
        "PE data directories in '$Path'"
    $importTableRva = if ($dataDirectoryCount -gt 1) {
        [BitConverter]::ToUInt32($bytes, $dataDirectoryOffset + 8)
    }
    else {
        [uint32]0
    }
    $delayImportTableRva = if ($dataDirectoryCount -gt 13) {
        [BitConverter]::ToUInt32($bytes, $dataDirectoryOffset + 104)
    }
    else {
        [uint32]0
    }

    $sectionTableOffset = $optionalHeaderOffset + $optionalHeaderSize
    $sections = @()
    for ($index = 0; $index -lt $sectionCount; ++$index) {
        $sectionOffset = $sectionTableOffset + $index * 40
        Assert-PeRange `
            $bytes `
            $sectionOffset `
            40 `
            "PE section header in '$Path'"
        $sections += [pscustomobject]@{
            VirtualSize = [BitConverter]::ToUInt32($bytes, $sectionOffset + 8)
            VirtualAddress = [BitConverter]::ToUInt32($bytes, $sectionOffset + 12)
            RawSize = [BitConverter]::ToUInt32($bytes, $sectionOffset + 16)
            RawOffset = [BitConverter]::ToUInt32($bytes, $sectionOffset + 20)
        }
    }

    $imports = @()
    if ($importTableRva -ne 0) {
        $importTableOffset = Convert-PeRvaToFileOffset `
            -Rva $importTableRva `
            -Sections $sections `
            -SizeOfHeaders $sizeOfHeaders `
            -FileLength $bytes.Length
        $terminated = $false
        for ($index = 0; $index -lt 4096; ++$index) {
            $descriptorOffset = $importTableOffset + $index * 20
            Assert-PeRange `
                $bytes `
                $descriptorOffset `
                20 `
                "PE import descriptor in '$Path'"
            $descriptorFields = @(
                [BitConverter]::ToUInt32($bytes, $descriptorOffset),
                [BitConverter]::ToUInt32($bytes, $descriptorOffset + 4),
                [BitConverter]::ToUInt32($bytes, $descriptorOffset + 8),
                [BitConverter]::ToUInt32($bytes, $descriptorOffset + 12),
                [BitConverter]::ToUInt32($bytes, $descriptorOffset + 16)
            )
            if (@($descriptorFields | Where-Object { $_ -ne 0 }).Count -eq 0) {
                $terminated = $true
                break
            }

            $nameOffset = Convert-PeRvaToFileOffset `
                -Rva $descriptorFields[3] `
                -Sections $sections `
                -SizeOfHeaders $sizeOfHeaders `
                -FileLength $bytes.Length
            $imports += Get-PeAsciiString `
                -Bytes $bytes `
                -Offset $nameOffset `
                -Path $Path `
                -Description 'PE import name'
        }
        if (-not $terminated) {
            throw "[SF-PE-PARSE-IMPORTS] PE import table contains too many descriptors in '$Path'."
        }
    }

    if ($delayImportTableRva -ne 0) {
        $delayImportTableOffset = Convert-PeRvaToFileOffset `
            -Rva $delayImportTableRva `
            -Sections $sections `
            -SizeOfHeaders $sizeOfHeaders `
            -FileLength $bytes.Length
        $terminated = $false
        for ($index = 0; $index -lt 4096; ++$index) {
            $descriptorOffset = $delayImportTableOffset + $index * 32
            Assert-PeRange `
                $bytes `
                $descriptorOffset `
                32 `
                "PE delay-import descriptor in '$Path'"
            $descriptorFields = @(
                0..7 | ForEach-Object {
                    [BitConverter]::ToUInt32(
                        $bytes,
                        $descriptorOffset + $_ * 4)
                }
            )
            if (@($descriptorFields | Where-Object { $_ -ne 0 }).Count -eq 0) {
                $terminated = $true
                break
            }

            $nameValue = [uint64]$descriptorFields[1]
            $nameRvaValue = if (($descriptorFields[0] -band 1) -ne 0) {
                $nameValue
            }
            else {
                if ($nameValue -lt [uint64]$optionalLayout.ImageBase) {
                    throw "[SF-PE-PARSE-DELAY] PE delay-import name VA resolves below the image base in '$Path'."
                }
                $nameValue - [uint64]$optionalLayout.ImageBase
            }
            if ($nameRvaValue -gt [uint64][uint32]::MaxValue) {
                throw "[SF-PE-PARSE-DELAY] PE delay-import name RVA is too large in '$Path'."
            }
            $nameOffset = Convert-PeRvaToFileOffset `
                -Rva ([uint32]$nameRvaValue) `
                -Sections $sections `
                -SizeOfHeaders $sizeOfHeaders `
                -FileLength $bytes.Length
            $imports += Get-PeAsciiString `
                -Bytes $bytes `
                -Offset $nameOffset `
                -Path $Path `
                -Description 'PE delay-import name'
        }
        if (-not $terminated) {
            throw "[SF-PE-PARSE-DELAY] PE delay-import table contains too many descriptors in '$Path'."
        }
    }

    return @($imports | Sort-Object -Unique)
}

function Assert-StaticCfitsioPeImports {
    param(
        [Parameter(Mandatory = $true)] [string]$ExecutablePath,
        [Parameter(Mandatory = $true)] [string]$Description
    )

    $imports = @(Get-PeImportedDllNames -Path $ExecutablePath)
    if ($imports.Count -eq 0) {
        throw "[SF-PE-IMPORTS-EMPTY] $Description has no readable PE imports."
    }
    $pathQualifiedImports = @(
        $imports | Where-Object { $_ -match '[\\/]' }
    )
    if ($pathQualifiedImports.Count -ne 0) {
        throw "[SF-PE-IMPORTS-BASENAME] $Description import names must be basenames without path separators: $($pathQualifiedImports -join ', ')."
    }
    $forbiddenImports = @(
        $imports | Where-Object {
            [IO.Path]::GetFileName($_) -match
                '(?i)^.*(?:cfitsio|curl|bzip|bz2).*\.dll$'
        }
    )
    if ($forbiddenImports.Count -ne 0) {
        throw "[SF-PE-IMPORTS-STATIC] $Description imports forbidden CFITSIO or optional-feature runtime DLLs: $($forbiddenImports -join ', ')."
    }
}
