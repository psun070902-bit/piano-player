# Piano Player

[English documentation](README.md)

**[한국어판 다운로드 (Windows x64)](https://github.com/psun070902-bit/piano-player/raw/refs/heads/main/downloads/v1.0.0/piano-player-1.0.0-windows-x64-ko.zip)** · **[Download English](https://github.com/psun070902-bit/piano-player/raw/refs/heads/main/downloads/v1.0.0/piano-player-1.0.0-windows-x64-en.zip)** · [SHA-256 검증값](downloads/v1.0.0/SHA256SUMS.txt)

Windows x64용 피아노 플레이어입니다. 한국어판과 영어판은 같은 C 연주 엔진을 사용하며, 호환되는 Roblox 피아노에 키보드 입력을 보내거나 설정된 VRChat 피아노에 Windows MIDI를 보냅니다.

## 다운로드 및 실행

1. 위의 다운로드 링크에서 한국어판 `windows-x64-ko.zip` 또는 영어판 `windows-x64-en.zip`을 받습니다. 공개 파일이므로 다운로드에 GitHub 계정이 필요하지 않습니다.
2. ZIP 전체를 압축 해제합니다. 실행 파일 옆의 `songs` 폴더를 유지해 주세요.
3. 사용 권한이 있는 `.mid`, `.midi`, 지원되는 `.vps` 파일을 `songs` 폴더에 넣습니다.
4. `piano_native_gui.exe`를 실행하여 **새로고침**을 누르고, 곡과 출력 방식을 선택한 뒤 **재생**합니다.

설치 프로그램·Python·곡 라이브러리·MP3 자동 채보 도구·가상 MIDI 드라이버·게임 변조 기능은 포함되어 있지 않습니다. MP3 직접 재생은 지원하지 않습니다. 실행 파일은 서명되지 않았으므로 다운로드 출처와 SHA-256 값을 확인해 주세요. 신뢰할 수 없는 파일을 실행하려고 보안 기능을 끄지 마세요.

## 출력 방식과 조작

**Roblox:** 호환되는 게임 피아노를 열고 건반 입력을 활성화해 주세요. 제목에 `Roblox`가 포함된 보이는 창을 대상으로 합니다. 다른 창으로 이동하면 눌린 키를 해제하고 일시정지하며, Roblox로 돌아오면 이어집니다. Roblox 창에서 **F8**로 직접 일시정지·이어 연주할 수 있습니다. 다른 창에 백그라운드 입력을 보내지는 않습니다. 키 매핑 범위는 MIDI 36~96(C2~C7)이며 기본적으로 범위 밖의 음을 옥타브 이동합니다. CLI의 `--no-fold`는 범위 밖 음을 제외합니다. 모든 피아노 게임과 호환되는 것은 아니며 키보드 입력으로는 MIDI 강약이나 CC64 페달 정보를 보낼 수 없습니다.

**VRChat MIDI:** 사용하시는 월드·피아노의 MIDI 연결을 먼저 설정하고 이름에 `VRChat Piano`가 포함된 Windows MIDI 출력 포트를 준비해 주세요. 환경에 따라 별도의 가상 MIDI 연결이 필요하지만 프로그램이 자동 설치하지는 않습니다. 다른 포트 이름은 `piano_native.exe --backend midi --port "포트 이름"`으로 지정할 수 있습니다. 다른 창을 사용해도 연주가 계속됩니다. 강약과 지원되는 제어 이벤트를 전송하지만 실제 소리는 받는 피아노에 따라 다릅니다. Roblox 전용 창 전환·F8 일시정지는 MIDI에 적용되지 않습니다.

**정지** 버튼, **F7**, **Esc**로 연주를 정지합니다. CLI 대기 중 **F6**는 재생, **F8**는 다음 곡, **F7/Esc**는 종료입니다. Roblox 연주 중 F8은 일시정지·이어 연주입니다. GUI에서는 버튼이나 곡 더블클릭을 기본 조작으로 사용해 주세요.

## 연주 표현

기본값은 **자연스럽게**입니다. 원본·은은하게·표현 풍부하게도 선택할 수 있습니다. 원본의 짧은 음·쉼표·페달을 존중하고 연타와 건반 충돌을 확인하며, 제한된 범위에서 타이밍·음 길이·강약을 보정합니다. 모든 음을 일괄적으로 늘리거나 페달을 계속 켜지 않습니다. 보정을 원하지 않으면 **원본**을 선택해 주세요.

첫 음은 속도와 무관하게 **재생 버튼 기준 1.2초**를 목표로 합니다. 파일 로딩·운영체제·게임의 지연으로 실제 소리가 늦어질 수 있습니다. 자동 연주이며 실제 사람이 실시간으로 치는 것은 아닙니다. 채보 오류 수정·모르는 곡 즉흥 연주·채팅 읽기·게임 노트 트레일의 모양까지 보장하는 기능은 없습니다. 자동 연주임을 투명하게 알리고 플랫폼·월드의 규칙을 지켜 주세요.

## 곡 분류와 정렬

`songs` 바로 아래 파일만 읽습니다. 같은 파일명에 여러 형식이 있으면 `.mid` → `.midi` → `.vps` 순으로 선택합니다. UTF-8 TSV 파일로 분류·이름·별칭을 설정할 수 있습니다. 구분자는 실제 탭이며 헤더는 필요하지 않습니다.

| 파일 | 열 순서 |
| --- | --- |
| `song_categories.tsv` | 분류 순서 숫자, 분류 이름, 확장자 없는 파일명 |
| `song_artists.tsv` | 확장자 없는 파일명, 아티스트/작곡가 |
| `song_titles_ko.tsv` | 확장자 없는 파일명, 표시 제목 (언어 무관) |
| `song_sort_keys.tsv` | 확장자 없는 파일명, 영문 `Artist - Title` 보조 키 |
| `song_search_aliases.tsv` | 확장자 없는 파일명, 검색 별칭 |

클래식·J-POP·K-POP·OST 등 지정한 분류 순서를 먼저 유지하고 그 안에서 아티스트/작곡가·제목 순으로 정렬합니다. ABC 정렬에는 영문 아티스트 이름을 사용해 주세요. 아티스트 정보나 `Artist - Title` 보조 키가 없으면 미분류로 해당 분류 마지막에 표시됩니다. 작곡가와 커버 연주자의 크레딧은 정확하게 입력해 주세요. 사용자 곡 제목과 분류는 영어판에서도 자동 번역하지 않습니다.

## 소스에서 빌드

Windows x64, PowerShell, x64 MinGW-w64 GCC(예: MSYS2 UCRT64)가 필요합니다. 저장소 폴더에서 실행합니다.

```powershell
.\build.ps1 -Compiler C:\msys64\ucrt64\bin\gcc.exe
.\test.ps1 -Compiler C:\msys64\ucrt64\bin\gcc.exe
.\package.ps1 -Compiler C:\msys64\ucrt64\bin\gcc.exe
```

컴파일러 경로는 예시이며 설치 위치에 맞게 지정합니다. PATH에 `gcc`가 있으면 `-Compiler`를 생략할 수 있습니다. 결과는 `build/ko`, `build/en`이며 배포 ZIP은 `dist`에 생성됩니다. `PN_LANG_EN`으로 언어를 선택하므로 연주 엔진이 따로 나뉘지 않습니다.

검사는 실제 게임 키보드 입력이나 MIDI를 보내지 않는 모의 출력 검사와 숨김 GUI 검사로 진행합니다. `test.ps1`의 선택 옵션 `-SongsDirectory`로 개인 곡 폴더를 로컬에서 검사할 수 있으며 업로드하지 않습니다. 일부 저수준 오류 진단은 한국어판에서도 영어로 표시됩니다.

## 배포 범위

프로그램 소스·문서·테스트·정리된 실행 파일만 배포하며 개인 곡·음원은 포함하지 않습니다. 사용 권한이 있는 파일만 사용해 주세요. 라이선스 상태는 [DISTRIBUTION.md](DISTRIBUTION.md), 변경 내역은 [CHANGELOG.md](CHANGELOG.md)를 참고해 주세요. Roblox·VRChat·음악 권리자와 제휴한 프로젝트가 아닙니다.
