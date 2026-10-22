[CmdletBinding()]
param([string]$Compiler = 'gcc')
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$compilerPath = (Get-Command $Compiler -ErrorAction Stop).Source
$common = @('common.c','humanize.c','midi_parser.c','vps_parser.c','song_list.c','win_player.c') |
    ForEach-Object { Join-Path $root "src\$_" }
$flags = @('-std=c11','-O2','-Wall','-Wextra','-Wpedantic','-DUNICODE','-D_UNICODE','-static-libgcc')
foreach ($language in @('ko','en')) {
    $out = Join-Path $root "build\$language"
    New-Item -ItemType Directory -Force -Path "$out\songs" | Out-Null
    $languageFlags = @()
    if ($language -eq 'en') { $languageFlags = @('-DPN_LANG_EN') }
    & $compilerPath @flags @languageFlags (Join-Path $root 'src\main.c') @common '-o' "$out\piano_native.exe" '-municode' '-lwinmm'
    if ($LASTEXITCODE -ne 0) { throw "Console compilation failed: $language" }
    & $compilerPath @flags @languageFlags (Join-Path $root 'src\gui_main.c') @common '-o' "$out\piano_native_gui.exe" '-municode' '-mwindows' '-lwinmm' '-lcomctl32'
    if ($LASTEXITCODE -ne 0) { throw "GUI compilation failed: $language" }
    Copy-Item -LiteralPath "$root\songs\README.md" -Destination "$out\songs\README.md" -Force
}
Write-Host 'Built Korean and English editions in build/ko and build/en.'
