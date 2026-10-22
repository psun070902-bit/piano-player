[CmdletBinding()]
param([string]$Compiler = 'gcc', [string]$SongsDirectory = '')
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$compilerPath = (Get-Command $Compiler -ErrorAction Stop).Source
& "$root\build.ps1" -Compiler $compilerPath
$testOut = Join-Path $root 'build\tests'
$fixture = Join-Path $root 'build\test-songs'
New-Item -ItemType Directory -Force -Path $testOut,$fixture | Out-Null

# Original, generated test pattern, not a third-party song or a release asset.
$track = [Collections.Generic.List[byte]]::new()
function Add-Event([int[]]$Values) { foreach ($value in $Values) { $track.Add([byte]$value) } }
Add-Event @(0,255,81,3,7,161,32) # 120 BPM, 480 ticks/beat
Add-Event @(0,144,60,82,0,144,64,65,0,144,67,76)
Add-Event @(129,112,128,60,0,0,128,64,0,0,128,67,0)
Add-Event @(0,176,64,127,0,144,62,78,120,128,62,0)
Add-Event @(0,144,62,72,120,128,62,0,0,176,64,0)
Add-Event @(0,144,65,80,129,112,128,65,0,0,255,47,0)
$midi = [Collections.Generic.List[byte]]::new()
foreach ($b in @(77,84,104,100,0,0,0,6,0,0,0,1,1,224,77,84,114,107,0,0,0,$track.Count)) { $midi.Add([byte]$b) }
$midi.AddRange($track)
[IO.File]::WriteAllBytes("$fixture\original-test.mid", $midi.ToArray())

$cases = @{
    test_continuity = @('common.c','midi_parser.c','vps_parser.c','song_list.c')
    test_expression = @('common.c','midi_parser.c','vps_parser.c','song_list.c')
    test_release_tail = @('humanize.c','midi_parser.c','vps_parser.c','song_list.c')
    test_focus_resume = @('common.c','humanize.c','midi_parser.c','vps_parser.c','song_list.c')
    test_human = @('common.c','humanize.c','midi_parser.c','vps_parser.c','song_list.c')
    test_key_collisions = @('common.c','humanize.c','midi_parser.c','vps_parser.c','song_list.c')
}
function Invoke-Checked([string]$Program, [string[]]$Arguments = @()) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
foreach ($name in ($cases.Keys | Sort-Object)) {
    $sources = @($cases[$name] | ForEach-Object { Join-Path $root "src\$_" })
    $entryFlags = @()
    if ($name -in @('test_human','test_key_collisions')) { $entryFlags = @('-municode') }
    & $compilerPath '-std=c11' '-O2' '-Wall' '-Wextra' '-Wpedantic' "-I$root\src" "$root\tests\$name.c" @sources '-o' "$testOut\$name.exe" @entryFlags '-lwinmm'
    if ($LASTEXITCODE -ne 0) { throw "Test compilation failed: $name" }
    if ($name -notin @('test_human','test_key_collisions')) { Invoke-Checked "$testOut\$name.exe" }
}
Invoke-Checked "$testOut\test_human.exe" @($fixture)
Invoke-Checked "$testOut\test_human.exe" @($fixture,'--midi')
Invoke-Checked "$testOut\test_key_collisions.exe" @($fixture)
foreach ($language in @('ko','en')) {
    $dir = Join-Path $root "build\$language"
    # Hidden GUI tests: never click Play and never send input/MIDI.
    $p = Start-Process -FilePath "$dir\piano_native_gui.exe" -ArgumentList '--smoke-test' -WindowStyle Hidden -PassThru -Wait
    if ($p.ExitCode -ne 0) { throw "Empty-library GUI test failed: $language ($($p.ExitCode))" }
    Copy-Item -LiteralPath "$fixture\original-test.mid" -Destination "$dir\songs\original-test.mid" -Force
    try {
        $p = Start-Process -FilePath "$dir\piano_native_gui.exe" -ArgumentList '--smoke-test' -WindowStyle Hidden -PassThru -Wait
        if ($p.ExitCode -ne 0) { throw "Populated GUI test failed: $language ($($p.ExitCode))" }
        Invoke-Checked "$dir\piano_native.exe" @('--validate-all','--quiet')
        Invoke-Checked "$dir\piano_native.exe" @('--validate-all','--quiet','--backend','midi')
        Invoke-Checked "$dir\piano_native.exe" @('--help')
    } finally {
        # One exact generated fixture, never a user library or recursive deletion.
        Remove-Item -LiteralPath "$dir\songs\original-test.mid" -ErrorAction SilentlyContinue
    }
}
if ($SongsDirectory) {
    $library = (Resolve-Path -LiteralPath $SongsDirectory).Path
    Invoke-Checked "$testOut\test_human.exe" @($library)
    Invoke-Checked "$testOut\test_human.exe" @($library,'--midi')
    Invoke-Checked "$testOut\test_key_collisions.exe" @($library)
}
Write-Host 'All tests passed. No real keyboard or MIDI output was sent.'
