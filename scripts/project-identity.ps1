# Shared with CMake/C++; each property is an independent contract.
$ProjectIdentity = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot '../config/project_identity.json') | ConvertFrom-Json
$ApplicationId = $ProjectIdentity.founding_identity
$ArtifactFileName = $ProjectIdentity.artifact_basename + '.exe'
$MetadataFileName = $ProjectIdentity.metadata_filename
