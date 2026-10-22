# Piano Player

[한국어 설명서](README.ko.md)

A Windows x64 piano player with Korean and English interfaces. It sends keyboard input to a compatible Roblox piano or Windows MIDI messages to a configured VRChat piano setup. Both editions use the same C engine.

## Download and start

1. Open [Releases](https://github.com/psun070902-bit/piano-player/releases) and download the `windows-x64-en.zip` (English) or `windows-x64-ko.zip` (Korean) asset.
2. Extract the whole ZIP. Keep the `songs` folder beside the executables.
3. Add your own authorized `.mid`, `.midi` or supported `.vps` files to `songs`.
4. Run `piano_native_gui.exe`, click **Refresh**, select a song and an output mode, then **Play**.

No installer, Python, music library, MP3-to-MIDI transcriber, virtual MIDI driver or game modification is included. MP3 files cannot be played directly. Builds are unsigned; verify the download source and SHA-256 checksum. Do not disable security protections to run a download you do not trust.

## Output modes

**Roblox:** open a compatible piano and enable its keyboard input before playing. The player targets a visible window whose title contains `Roblox`. Switching away pauses the score and releases held keys; returning resumes. Press **F8** in the Roblox window to manually pause/resume. It does not inject background input into other windows. The keyboard mapping covers MIDI 36–96 (C2–C7); out-of-range notes are octave-folded by default, or skipped with CLI `--no-fold`. This mapping is not compatible with every piano game. Keyboard input cannot transmit MIDI velocity or sustain CC64.

**VRChat MIDI:** configure your MIDI-capable piano/world and a Windows MIDI output port containing `VRChat Piano` before using the GUI. An external virtual MIDI connection may be required by your setup; it is not installed automatically. For another port name, use `piano_native.exe --backend midi --port "Your port"`. MIDI playback continues when another window is active. Velocity and supported controller events are sent; audible results depend on the receiving piano. Roblox's focus/F8 pause behavior does not apply to MIDI.

Use the **Stop** button, **F7** or **Esc** to stop playback. F8 means **next song while idle** in the CLI; while Roblox is playing it means **pause/resume**. CLI F6 starts the selected song. GUI buttons/double-click are the primary controls.

## Musical expression

The default style is **Natural**. Original, Subtle and Expressive are also available. The engine applies bounded timing, duration and voicing adjustments with repeated-note and key-collision guards. It respects short notes, rests and source pedal events; it does not simply extend every note or leave sustain on. Use Original when you do not want expression adjustments. The target first note is **1.2 seconds after Play**, independent of playback speed; loading, operating-system scheduling and the receiving game may add latency.

This is automated playback, not live human performance. Expression processing does not fix transcription errors, improvise unknown songs, read chat, or guarantee how a game's note trails look. Be transparent about automation and follow each platform/world's rules.

## Organizing your library

Files are read from the top level of `songs`, not recursively. If multiple formats share a filename stem, `.mid` takes priority over `.midi`, then `.vps`. Optional UTF-8 TSV files (actual tab separators, no required header) keep existing library compatibility:

| File | Columns |
| --- | --- |
| `song_categories.tsv` | numeric category order, category name, filename stem |
| `song_artists.tsv` | filename stem, artist/composer |
| `song_titles_ko.tsv` | filename stem, display title (any language; legacy filename) |
| `song_sort_keys.tsv` | filename stem, English `Artist - Title` fallback key |
| `song_search_aliases.tsv` | filename stem, search aliases |

Category order is preserved first, then artist/composer and title are sorted case-insensitively within each category. Use English artist names for ABC grouping. Without artist metadata or an `Artist - Title` fallback, entries use Unknown artist and sort last within their category. Credit composers and cover performers accurately. User song titles and metadata are not automatically translated between editions.

## Build from source

Requires Windows x64, PowerShell and an x64 MinGW-w64 GCC toolchain (for example MSYS2 UCRT64). In PowerShell from the repository directory:

```powershell
.\build.ps1 -Compiler C:\msys64\ucrt64\bin\gcc.exe
.\test.ps1 -Compiler C:\msys64\ucrt64\bin\gcc.exe
.\package.ps1 -Compiler C:\msys64\ucrt64\bin\gcc.exe
```

The compiler path is an example, not a required installation path. If `gcc` is on PATH, omit `-Compiler`. Output: `build/ko`, `build/en`, and release ZIPs in `dist`. Both source editions are selected by `PN_LANG_EN`; there are no separate playback engines. `test.ps1` runs simulated output tests and hidden GUI checks without sending real keyboard input or MIDI. Optional `-SongsDirectory` validates a private library locally; no music is uploaded by this script. Some low-level diagnostics remain English in the Korean edition.

## Rights and distribution

Only program source, documentation, tests and clean binaries are distributed. No personal music library or audio files are included. Use only files you are authorized to use. See [DISTRIBUTION.md](DISTRIBUTION.md) for the current license status and [CHANGELOG.md](CHANGELOG.md) for release notes. This project is not affiliated with Roblox, VRChat, or any music rights holder.
