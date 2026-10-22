[CmdletBinding()]
param([string]$Version = '1.0.0', [string]$Compiler = 'gcc')
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?$') { throw 'Invalid version.' }
& "$PSScriptRoot\build.ps1" -Compiler $Compiler
$dist = Join-Path $PSScriptRoot 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$checksums = @()
foreach ($language in @('ko','en')) {
    $build = Join-Path $PSScriptRoot "build\$language"
    $stage = Join-Path $PSScriptRoot "build\package-$Version-$language"
    if (Test-Path -LiteralPath $stage) { throw "Use a fresh package folder: $stage" }
    New-Item -ItemType Directory -Path "$stage\songs" | Out-Null
    # Exact allowlist: never copy the user's music library or arbitrary build files.
    foreach ($name in @('piano_native.exe','piano_native_gui.exe')) {
        Copy-Item -LiteralPath "$build\$name" -Destination "$stage\$name"
    }
    Copy-Item -LiteralPath "$PSScriptRoot\songs\README.md" -Destination "$stage\songs\README.md"
    foreach ($name in @('README.md','README.ko.md','CHANGELOG.md','DISTRIBUTION.md')) {
        Copy-Item -LiteralPath "$PSScriptRoot\$name" -Destination "$stage\$name"
    }
    $archive = Join-Path $dist "piano-player-$Version-windows-x64-$language.zip"
    if (Test-Path -LiteralPath $archive) { throw "Archive already exists: $archive" }
    Compress-Archive -LiteralPath (Get-ChildItem -LiteralPath $stage).FullName -DestinationPath $archive
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
    $checksums += "$hash  $([IO.Path]::GetFileName($archive))"
}
$checksums | Set-Content -LiteralPath "$dist\SHA256SUMS.txt" -Encoding ascii
Write-Host "Release packages created in $dist"
