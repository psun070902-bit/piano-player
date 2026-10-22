#include "piano_native.h"

#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void pn_print_help(void) {
    wprintf(
        PN_TEXT(L"Windows C 피아노 플레이어\n\n", L"Native C piano player for Windows\n\n")
        PN_TEXT(L"사용법:\n", L"Usage:\n")
        L"  piano_native.exe [song.mid|song.vps] [options]\n\n"
        PN_TEXT(L"옵션:\n", L"Options:\n")
        PN_TEXT(L"  --backend roblox|midi   출력 방식 (기본: roblox)\n", L"  --backend roblox|midi   Output type (default: roblox)\n")
        PN_TEXT(L"  --port NAME             Windows MIDI 출력 포트 (기본: VRChat Piano)\n", L"  --port NAME             Windows MIDI output (default: VRChat Piano)\n")
        PN_TEXT(L"  --window TITLE          Roblox 창 제목 (기본: Roblox)\n", L"  --window TITLE          Required Roblox window title (default: Roblox)\n")
        PN_TEXT(L"  --songs DIR             곡 폴더 경로\n", L"  --songs DIR             Song library directory\n")
        PN_TEXT(L"  --speed NUMBER          재생 속도 배율\n", L"  --speed NUMBER          Playback speed multiplier\n")
        PN_TEXT(L"  --bpm NUMBER            기준 BPM 변경\n", L"  --bpm NUMBER            Override the base BPM\n")
        PN_TEXT(L"  --transpose NUMBER      반음 단위 조옮김\n", L"  --transpose NUMBER      Semitone transpose\n")
        PN_TEXT(L"  --from-bar NUMBER       시작 마디\n", L"  --from-bar NUMBER       Start from this bar\n")
        PN_TEXT(L"  --hold MS               Roblox 기본 타건 길이 (기본: 20ms)\n", L"  --hold MS               Roblox key hold time (default: 20)\n")
        PN_TEXT(L"  --humanize 0..3         원본/은은하게/자연스럽게/표현 풍부하게 (기본: 2)\n", L"  --humanize 0..3         Original/subtle/natural/expressive (default: 2)\n")
        PN_TEXT(L"  --human-seed NUMBER     같은 연주 보정을 재현할 시드\n", L"  --human-seed NUMBER     Repeatable human performance seed\n")
        PN_TEXT(L"  --no-fold               C2~C7 범위 밖의 음 제외\n", L"  --no-fold               Skip notes outside C2-C7\n")
        PN_TEXT(L"  --keep-leading-silence  시작 위치 정규화 전 선행 제어 이벤트 유지\n", L"  --keep-leading-silence  Preserve lead-in controls before normalization\n")
        PN_TEXT(L"  --trim-leading-silence  원본 앞부분 정리 후 첫 음 1.2초 대기\n", L"  --trim-leading-silence  Trim source opening before 1.2 s start delay\n")
        PN_TEXT(L"  --dry-run               실제 입력 없이 분석 및 미리보기\n", L"  --dry-run               Parse and preview without sending input\n")
        PN_TEXT(L"  --validate-all          곡 폴더 전체 분석 검사\n", L"  --validate-all          Parse every song in the library\n")
        PN_TEXT(L"  --list                   정렬된 곡 목록 표시 후 종료\n", L"  --list                   Show the sorted song list and exit\n")
        PN_TEXT(L"  --quiet                  진행 상세 숨김\n", L"  --quiet                  Hide progress details\n")
        PN_TEXT(L"  --help                   도움말 표시\n\n", L"  --help                   Show this help\n\n")
        PN_TEXT(L"대기 중: F6 재생, F8 다음 곡, F7/ESC 종료.\n", L"Idle: F6 play, F8 next song, F7/ESC exit.\n")
        PN_TEXT(L"Roblox 연주 중: F8 일시정지/이어 연주, F7/ESC 정지.\n", L"During Roblox playback: F8 pause/resume; F7/ESC stop.\n")
    );
}

static bool pn_parse_double(const wchar_t *text, double *value) {
    wchar_t *end;
    double parsed = wcstod(text, &end);
    if (end == text || *end) return false;
    *value = parsed;
    return true;
}

static bool pn_parse_int(const wchar_t *text, int *value) {
    wchar_t *end;
    long parsed = wcstol(text, &end, 10);
    if (end == text || *end || parsed < INT_MIN || parsed > INT_MAX) return false;
    *value = (int)parsed;
    return true;
}

static void pn_default_songs_dir(wchar_t *directory, size_t cap) {
    wchar_t module[PN_PATH_CAP];
    DWORD length = GetModuleFileNameW(NULL, module, PN_PATH_CAP);
    if (!length || length >= PN_PATH_CAP) {
        pn_copy_wide(directory, cap, L"songs");
        return;
    }
    wchar_t *slash = wcsrchr(module, L'\\');
    if (slash) *slash = L'\0';
    _snwprintf_s(directory, cap, _TRUNCATE, L"%ls\\songs", module);
}

static bool pn_full_path(const wchar_t *input, wchar_t *output, size_t cap) {
    DWORD result = GetFullPathNameW(input, (DWORD)cap, output, NULL);
    return result > 0 && result < cap;
}

static bool pn_parse_options(int argc, wchar_t **argv, PnOptions *options,
                             wchar_t *error, size_t cap, bool *show_help) {
    memset(options, 0, sizeof(*options));
    options->speed = 1.0;
    options->hold_seconds = 0.020;
    options->start_delay = PN_DEFAULT_START_DELAY;
    options->humanize = 2;
    options->human_seed = GetTickCount();
    options->from_bar = 1;
    options->fold = true;
    options->keep_leading_silence = true;
    pn_copy_wide(options->port_query, PN_TITLE_CAP, L"VRChat Piano");
    pn_copy_wide(options->window_query, PN_TITLE_CAP, L"Roblox");
    pn_default_songs_dir(options->songs_dir, PN_PATH_CAP);
    *show_help = false;

    int index;
    for (index = 1; index < argc; ++index) {
        const wchar_t *argument = argv[index];
        if (_wcsicmp(argument, L"--help") == 0 || _wcsicmp(argument, L"-h") == 0) {
            *show_help = true;
            return true;
        }
        if (_wcsicmp(argument, L"--dry-run") == 0) options->dry_run = true;
        else if (_wcsicmp(argument, L"--validate-all") == 0) options->validate_all = true;
        else if (_wcsicmp(argument, L"--list") == 0) options->list_only = true;
        else if (_wcsicmp(argument, L"--quiet") == 0) options->quiet = true;
        else if (_wcsicmp(argument, L"--no-fold") == 0) options->fold = false;
        else if (_wcsicmp(argument, L"--keep-leading-silence") == 0)
            options->keep_leading_silence = true;
        else if (_wcsicmp(argument, L"--trim-leading-silence") == 0)
            options->keep_leading_silence = false;
        else if (_wcsicmp(argument, L"--backend") == 0) {
            if (++index >= argc) goto missing_value;
            if (_wcsicmp(argv[index], L"midi") == 0) options->backend_midi = true;
            else if (_wcsicmp(argv[index], L"roblox") == 0) options->backend_midi = false;
            else {
                pn_set_error(error, cap, L"Unknown backend: %ls", argv[index]);
                return false;
            }
        } else if (_wcsicmp(argument, L"--port") == 0) {
            if (++index >= argc) goto missing_value;
            pn_copy_wide(options->port_query, PN_TITLE_CAP, argv[index]);
        } else if (_wcsicmp(argument, L"--window") == 0) {
            if (++index >= argc) goto missing_value;
            pn_copy_wide(options->window_query, PN_TITLE_CAP, argv[index]);
        } else if (_wcsicmp(argument, L"--songs") == 0) {
            if (++index >= argc) goto missing_value;
            if (!pn_full_path(argv[index], options->songs_dir, PN_PATH_CAP)) {
                pn_set_error(error, cap, L"Invalid songs directory path.");
                return false;
            }
        } else if (_wcsicmp(argument, L"--speed") == 0) {
            if (++index >= argc) goto missing_value;
            if (!pn_parse_double(argv[index], &options->speed) || options->speed <= 0.0) {
                pn_set_error(error, cap, L"--speed must be greater than zero.");
                return false;
            }
        } else if (_wcsicmp(argument, L"--bpm") == 0) {
            if (++index >= argc) goto missing_value;
            if (!pn_parse_double(argv[index], &options->bpm_override) ||
                options->bpm_override <= 0.0) {
                pn_set_error(error, cap, L"--bpm must be greater than zero.");
                return false;
            }
        } else if (_wcsicmp(argument, L"--transpose") == 0) {
            if (++index >= argc) goto missing_value;
            if (!pn_parse_int(argv[index], &options->transpose) ||
                options->transpose < -127 || options->transpose > 127) {
                pn_set_error(error, cap, L"--transpose must be between -127 and 127.");
                return false;
            }
        } else if (_wcsicmp(argument, L"--from-bar") == 0) {
            if (++index >= argc) goto missing_value;
            if (!pn_parse_int(argv[index], &options->from_bar) || options->from_bar < 1) {
                pn_set_error(error, cap, L"--from-bar must be at least 1.");
                return false;
            }
        } else if (_wcsicmp(argument, L"--humanize") == 0) {
            if (++index >= argc) goto missing_value;
            if (!pn_parse_int(argv[index], &options->humanize) ||
                options->humanize < 0 || options->humanize > 3) {
                pn_set_error(error, cap, L"--humanize must be between 0 and 3.");
                return false;
            }
        } else if (_wcsicmp(argument, L"--human-seed") == 0) {
            int seed;
            if (++index >= argc) goto missing_value;
            if (!pn_parse_int(argv[index], &seed) || seed < 0) {
                pn_set_error(error, cap, L"--human-seed must be a nonnegative integer.");
                return false;
            }
            options->human_seed = (uint32_t)seed;
        } else if (_wcsicmp(argument, L"--hold") == 0) {
            double milliseconds;
            if (++index >= argc) goto missing_value;
            if (!pn_parse_double(argv[index], &milliseconds) || milliseconds < 4.0 ||
                milliseconds > 1000.0) {
                pn_set_error(error, cap, L"--hold must be between 4 and 1000 ms.");
                return false;
            }
            options->hold_seconds = milliseconds / 1000.0;
        } else if (argument[0] == L'-') {
            pn_set_error(error, cap, L"Unknown option: %ls", argument);
            return false;
        } else if (!options->direct_song[0]) {
            if (!pn_full_path(argument, options->direct_song, PN_PATH_CAP)) {
                pn_set_error(error, cap, L"Invalid song path: %ls", argument);
                return false;
            }
        } else {
            pn_set_error(error, cap, L"Only one song path can be specified.");
            return false;
        }
    }
    return true;

missing_value:
    pn_set_error(error, cap, L"Option %ls needs a value.", argv[index - 1]);
    return false;
}

static size_t pn_note_on_count(const PnSong *song) {
    size_t count = 0;
    for (size_t index = 0; index < song->event_count; ++index) {
        if (song->events[index].type == PN_NOTE_ON) ++count;
    }
    return count;
}

static void pn_describe(const PnSong *song, const wchar_t *path,
                        const PnOptions *options) {
    wprintf(PN_TEXT(L"\n  곡       : %ls\n", L"\n  Song     : %ls\n"), song->title);
    wprintf(PN_TEXT(L"  파일     : %ls\n", L"  File     : %ls\n"), path);
    wprintf(PN_TEXT(L"  박자     : %d/%d   BPM %.3f   속도 x%.3f\n", L"  Meter    : %d/%d   BPM %.3f   speed x%.3f\n"),
            song->meter_num, song->meter_den, song->bpm, options->speed);
    wprintf(PN_TEXT(L"  내용     : %zu음, %zu 이벤트, %.1f초\n", L"  Content  : %zu note-ons, %zu timed events, %.1f seconds\n"),
            pn_note_on_count(song), song->event_count, song->duration);
    wprintf(PN_TEXT(L"  원본     : %ls\n", L"  Source   : %ls\n"),
            song->performance_source ? PN_TEXT(L"MP3 연주 기반 MIDI", L"MP3 performance MIDI") : PN_TEXT(L"악보/MIDI", L"score/MIDI"));
    wprintf(PN_TEXT(L"  표현     : 단계 %d, 시드 %u%ls\n", L"  Human    : level %d, seed %u%ls\n"), options->humanize,
            (unsigned)options->human_seed,
            song->performance_source && options->humanize ? PN_TEXT(L" (원본 표현 유지: 보정 강도 55%)", L" (source expression preserved: 55% strength)") : L"");
}

static bool pn_prepare_song(const wchar_t *path, const PnOptions *options,
                            PnSong *song, wchar_t *error, size_t cap) {
    if (!pn_load_song(path, song, error, cap)) return false;
    if (!pn_apply_options(song, options, error, cap)) {
        pn_song_free(song);
        return false;
    }
    return true;
}

static int pn_validate_all(const PnSongList *list, const PnOptions *options) {
    size_t total_notes = 0;
    size_t total_events = 0;
    int failures = 0;
    for (size_t index = 0; index < list->count; ++index) {
        PnSong song;
        wchar_t error[PN_ERROR_CAP] = L"";
        if (!pn_prepare_song(list->items[index].path, options, &song,
                             error, PN_ERROR_CAP)) {
            fwprintf(stderr, L"  FAIL %zu) %ls\n       %ls\n",
                     index + 1, list->items[index].title, error);
            ++failures;
            continue;
        }
        size_t notes = pn_note_on_count(&song);
        if (!notes || song.events[0].time < -1e-9) {
            fwprintf(stderr, L"  FAIL %zu) %ls: invalid event schedule\n",
                     index + 1, song.title);
            ++failures;
        } else if (!options->quiet) {
            wprintf(L"  OK   %zu) %ls (%zu notes)\n",
                    index + 1, song.title, notes);
        }
        total_notes += notes;
        total_events += song.event_count;
        pn_song_free(&song);
    }
    wprintf(L"\n  Validation: songs=%zu failures=%d notes=%zu events=%zu\n",
            list->count, failures, total_notes, total_events);
    return failures ? 2 : 0;
}

static void pn_print_list(const PnSongList *list) {
    wprintf(PN_TEXT(L"\n  곡 목록\n", L"\n  Songs\n"));
    const wchar_t *previous_artist = NULL;
    const wchar_t *previous_category = NULL;
    for (size_t index = 0; index < list->count; ++index) {
        const wchar_t *artist = list->items[index].artist;
        const wchar_t *category = list->items[index].category;
        if (!previous_category || _wcsicmp(previous_category, category) != 0) {
            wprintf(L"\n  [%ls]\n", category);
            previous_category = category;
            previous_artist = NULL;
        }
        if (!previous_artist || _wcsicmp(previous_artist, artist) != 0) {
            wprintf(L"    %ls\n", artist);
            previous_artist = artist;
        }
        wprintf(L"    %zu) %ls\n", index + 1, list->items[index].title);
    }
}

static size_t pn_choose_song(const PnSongList *list) {
    pn_print_list(list);
    wchar_t input[128];
    for (;;) {
        wprintf(PN_TEXT(L"  곡 번호 (Enter = 1, q = 종료): ", L"  Number (Enter = 1, q = exit): "));
        fflush(stdout);
        if (!fgetws(input, 128, stdin)) return 0;
        wchar_t *cursor = input;
        while (*cursor && iswspace(*cursor)) ++cursor;
        if (!*cursor || *cursor == L'\r' || *cursor == L'\n') return 0;
        if (*cursor == L'q' || *cursor == L'Q') exit(0);
        wchar_t *end;
        unsigned long selected = wcstoul(cursor, &end, 10);
        while (*end && iswspace(*end)) ++end;
        if (!*end && selected >= 1 && selected <= list->count) return selected - 1;
        wprintf(PN_TEXT(L"  1~%zu 사이의 번호를 입력해 주세요.\n", L"  Enter a number from 1 to %zu.\n"), list->count);
    }
}

static void pn_preview(const PnSong *song) {
    wprintf(PN_TEXT(L"\n  앞 20개 이벤트\n", L"\n  First 20 events\n"));
    size_t limit = song->event_count < 20 ? song->event_count : 20;
    for (size_t index = 0; index < limit; ++index) {
        const PnEvent *event = &song->events[index];
        const wchar_t *kind = event->type == PN_NOTE_ON ? L"ON " :
                              event->type == PN_NOTE_OFF ? L"OFF" : L"CC ";
        wprintf(L"    %8.3f  %ls  %3u  %3u  bar %d\n",
                event->time, kind, event->data1, event->data2, event->bar);
    }
}

static bool pn_key_pressed(int vk) {
    return (GetAsyncKeyState(vk) & 1) != 0;
}

static void pn_wait_key_release(int vk) {
    while (GetAsyncKeyState(vk) & 0x8000) Sleep(10);
}

static int pn_interactive(const PnSongList *list, size_t selected,
                          const PnOptions *options) {
    for (;;) {
        PnSong song;
        wchar_t error[PN_ERROR_CAP] = L"";
        if (!pn_prepare_song(list->items[selected].path, options, &song,
                             error, PN_ERROR_CAP)) {
            fwprintf(stderr, PN_TEXT(L"오류: %ls\n", L"Error: %ls\n"), error);
            return 2;
        }
        pn_describe(&song, list->items[selected].path, options);
        wprintf(PN_TEXT(L"\n  F6 재생 | F8 다음 곡 | F7/ESC 종료\n", L"\n  F6 play once | F8 next song | F7/ESC exit\n"));
        if (!options->backend_midi) {
            wprintf(PN_TEXT(L"  '%ls' 창을 앞에 놓고 F6을 눌러 주세요.\n", L"  Put the '%ls' window in front before pressing F6.\n"),
                    options->window_query);
        }
        bool change_song = false;
        for (;;) {
            if (pn_key_pressed(VK_F7) || (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) {
                pn_song_free(&song);
                return 0;
            }
            if (pn_key_pressed(VK_F8) && list->count > 1) {
                pn_wait_key_release(VK_F8);
                selected = (selected + 1) % list->count;
                change_song = true;
                break;
            }
            if (pn_key_pressed(VK_F6)) {
                pn_wait_key_release(VK_F6);
                wprintf(PN_TEXT(L"\n  연주 중: %ls\n", L"\n  Playing: %ls\n"), song.title);
                bool ok = options->backend_midi
                    ? pn_play_midi(&song, options, error, PN_ERROR_CAP)
                    : pn_play_roblox(&song, options, error, PN_ERROR_CAP);
                if (ok) wprintf(PN_TEXT(L"  연주 완료.\n", L"  Playback finished.\n"));
                else wprintf(PN_TEXT(L"  연주 정지: %ls\n", L"  Playback stopped: %ls\n"), error);
                wprintf(PN_TEXT(L"\n  F6 재생 | F8 다음 곡 | F7/ESC 종료\n", L"\n  F6 play once | F8 next song | F7/ESC exit\n"));
            }
            Sleep(20);
        }
        pn_song_free(&song);
        if (!change_song) return 0;
    }
}

int wmain(int argc, wchar_t **argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    _wsetlocale(LC_ALL, L".UTF-8");
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
    _setmode(_fileno(stdin), _O_U8TEXT);

    PnOptions options;
    wchar_t error[PN_ERROR_CAP] = L"";
    bool show_help;
    if (!pn_parse_options(argc, argv, &options, error, PN_ERROR_CAP, &show_help)) {
        fwprintf(stderr, PN_TEXT(L"오류: %ls\n\n", L"Error: %ls\n\n"), error);
        pn_print_help();
        return 2;
    }
    if (show_help) {
        pn_print_help();
        return 0;
    }

    PnSongList list = {0};
    if (options.direct_song[0]) {
        list.items = (PnSongPath *)calloc(1, sizeof(PnSongPath));
        if (!list.items) {
            fwprintf(stderr, PN_TEXT(L"오류: 메모리가 부족합니다.\n", L"Error: not enough memory.\n"));
            return 2;
        }
        list.count = list.capacity = 1;
        pn_copy_wide(list.items[0].path, PN_PATH_CAP, options.direct_song);
        pn_copy_wide(list.items[0].category, PN_TITLE_CAP, PN_TEXT(L"직접 지정 파일", L"Direct file"));
        list.items[0].category_order = 0;
        PnSong title_song;
        if (pn_load_song(options.direct_song, &title_song, error, PN_ERROR_CAP)) {
            pn_copy_wide(list.items[0].title, PN_TITLE_CAP, title_song.title);
            pn_song_free(&title_song);
        } else {
            fwprintf(stderr, PN_TEXT(L"오류: %ls\n", L"Error: %ls\n"), error);
            pn_song_list_free(&list);
            return 2;
        }
    } else if (!pn_discover_songs(options.songs_dir, &list, error, PN_ERROR_CAP)) {
        fwprintf(stderr, PN_TEXT(L"오류: %ls\n", L"Error: %ls\n"), error);
        return 2;
    }

    if (options.list_only) {
        pn_print_list(&list);
        pn_song_list_free(&list);
        return 0;
    }
    if (options.validate_all) {
        int result = pn_validate_all(&list, &options);
        pn_song_list_free(&list);
        return result;
    }

    if (!list.count) {
        fwprintf(stderr, PN_TEXT(
            L"songs 폴더에 사용 권한이 있는 .mid, .midi 또는 .vps 파일을 넣어 주세요.\n",
            L"Add your own authorized .mid, .midi or .vps files to the songs folder.\n"));
        pn_song_list_free(&list);
        return 2;
    }

    size_t selected = options.direct_song[0] ? 0 : pn_choose_song(&list);
    if (options.dry_run) {
        PnSong song;
        if (!pn_prepare_song(list.items[selected].path, &options, &song,
                             error, PN_ERROR_CAP)) {
            fwprintf(stderr, PN_TEXT(L"오류: %ls\n", L"Error: %ls\n"), error);
            pn_song_list_free(&list);
            return 2;
        }
        pn_describe(&song, list.items[selected].path, &options);
        pn_preview(&song);
        pn_song_free(&song);
        pn_song_list_free(&list);
        return 0;
    }
    int result = pn_interactive(&list, selected, &options);
    pn_song_list_free(&list);
    return result;
}
