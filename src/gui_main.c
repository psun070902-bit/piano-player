#include "piano_native.h"

#include <commctrl.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define PN_GUI_CLASS L"PianoNativeGuiWindow"
#define PN_GUI_TITLE PN_TEXT(L"Piano Player", L"Piano Player - English")

#define IDC_SEARCH 1001
#define IDC_SONGS 1002
#define IDC_MODE_ROBLOX 1003
#define IDC_MODE_VRCHAT 1004
#define IDC_PLAY 1005
#define IDC_STOP 1006
#define IDC_PROGRESS 1007
#define IDC_TIME 1008
#define IDC_STATUS 1009
#define IDC_PREVIOUS 1010
#define IDC_NEXT 1011
#define IDC_REFRESH 1012
#define IDC_SPEED_LABEL 1013
#define IDC_SPEED 1014
#define IDC_AUTO_NEXT 1015
#define IDC_HUMAN_LABEL 1016
#define IDC_HUMAN 1017

#define WM_PN_PROGRESS (WM_APP + 1)
#define WM_PN_FINISHED (WM_APP + 2)
#define WM_PN_PAUSED (WM_APP + 3)

typedef struct PnGuiApp {
    HINSTANCE instance;
    HWND window;
    HWND search;
    HWND songs_view;
    HWND mode_roblox;
    HWND mode_vrchat;
    HWND play;
    HWND stop;
    HWND progress;
    HWND time;
    HWND status;
    HWND previous;
    HWND next;
    HWND refresh;
    HWND speed_label;
    HWND speed;
    HWND auto_next;
    HWND human_label;
    HWND human;
    HFONT font;
    PnSongList songs;
    wchar_t songs_dir[PN_PATH_CAP];
    HANDLE stop_event;
    HANDLE worker;
    bool playing;
    bool closing;
} PnGuiApp;

typedef struct PnPlaybackJob {
    PnGuiApp *app;
    size_t song_index;
    bool midi;
    double speed;
    int humanize;
    uint32_t human_seed;
    LONGLONG start_counter;
} PnPlaybackJob;

typedef struct PnPlaybackResult {
    bool success;
    bool stopped;
    wchar_t error[PN_ERROR_CAP];
} PnPlaybackResult;

static PnGuiApp app;

static double pn_gui_selected_speed(void) {
    static const double speeds[] = {0.75, 0.80, 0.90, 1.00, 1.10, 1.25};
    LRESULT selected = SendMessageW(app.speed, CB_GETCURSEL, 0, 0);
    if (selected < 0 || selected >= (LRESULT)(sizeof(speeds) / sizeof(speeds[0])))
        return 1.0;
    return speeds[selected];
}

static void pn_gui_default_options(PnOptions *options, bool midi) {
    memset(options, 0, sizeof(*options));
    options->speed = 1.0;
    options->hold_seconds = 0.020;
    options->start_delay = PN_DEFAULT_START_DELAY;
    options->from_bar = 1;
    options->fold = true;
    options->keep_leading_silence = true;
    options->quiet = true;
    options->backend_midi = midi;
    pn_copy_wide(options->port_query, PN_TITLE_CAP, L"VRChat Piano");
    pn_copy_wide(options->window_query, PN_TITLE_CAP, L"Roblox");
}

static bool pn_gui_directory_exists(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static void pn_gui_parent_directory(wchar_t *path) {
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash) *slash = L'\0';
}

static bool pn_gui_find_songs_dir(wchar_t *output, size_t cap) {
    wchar_t module[PN_PATH_CAP];
    DWORD length = GetModuleFileNameW(NULL, module, PN_PATH_CAP);
    if (!length || length >= PN_PATH_CAP) return false;
    pn_gui_parent_directory(module);

    wchar_t candidate[PN_PATH_CAP];
    if (_snwprintf_s(candidate, PN_PATH_CAP, _TRUNCATE,
                     L"%ls\\songs", module) >= 0 &&
        pn_gui_directory_exists(candidate)) {
        pn_copy_wide(output, cap, candidate);
        return true;
    }

    wchar_t nested[PN_PATH_CAP];
    if (_snwprintf_s(nested, PN_PATH_CAP, _TRUNCATE,
                     L"%ls\\..\\..\\songs", module) >= 0) {
        DWORD full = GetFullPathNameW(nested, PN_PATH_CAP, candidate, NULL);
        if (full > 0 && full < PN_PATH_CAP && pn_gui_directory_exists(candidate)) {
            pn_copy_wide(output, cap, candidate);
            return true;
        }
    }

    DWORD current = GetCurrentDirectoryW(PN_PATH_CAP, candidate);
    if (current > 0 && current < PN_PATH_CAP &&
        _snwprintf_s(nested, PN_PATH_CAP, _TRUNCATE,
                     L"%ls\\songs", candidate) >= 0 &&
        pn_gui_directory_exists(nested)) {
        pn_copy_wide(output, cap, nested);
        return true;
    }
    return false;
}

static void pn_gui_apply_font(HWND control) {
    SendMessageW(control, WM_SETFONT, (WPARAM)app.font, TRUE);
}

static void pn_gui_set_status(const wchar_t *text) {
    SetWindowTextW(app.status, text ? text : L"");
}

static void pn_gui_format_time(wchar_t *output, size_t cap,
                               unsigned elapsed_ms, unsigned total_ms) {
    unsigned elapsed = elapsed_ms / 1000;
    unsigned total = total_ms / 1000;
    _snwprintf_s(output, cap, _TRUNCATE, L"%02u:%02u / %02u:%02u",
                 elapsed / 60, elapsed % 60, total / 60, total % 60);
}

static void pn_gui_set_controls_enabled(bool enabled) {
    EnableWindow(app.search, enabled);
    EnableWindow(app.songs_view, enabled);
    EnableWindow(app.mode_roblox, enabled);
    EnableWindow(app.mode_vrchat, enabled);
    EnableWindow(app.play, enabled && ListView_GetItemCount(app.songs_view) > 0);
    EnableWindow(app.stop, !enabled);
    EnableWindow(app.previous, enabled);
    EnableWindow(app.next, enabled);
    EnableWindow(app.refresh, enabled);
    EnableWindow(app.speed, enabled);
    EnableWindow(app.auto_next, enabled);
    EnableWindow(app.human, enabled);
}

static size_t pn_gui_selected_song(void) {
    int row = ListView_GetNextItem(app.songs_view, -1, LVNI_SELECTED);
    if (row < 0) return SIZE_MAX;
    LVITEMW item;
    memset(&item, 0, sizeof(item));
    item.mask = LVIF_PARAM;
    item.iItem = row;
    if (!ListView_GetItem(app.songs_view, &item)) return SIZE_MAX;
    return (size_t)item.lParam;
}

static void pn_gui_populate_songs(void) {
    wchar_t query[PN_TITLE_CAP] = L"";
    GetWindowTextW(app.search, query, PN_TITLE_CAP);
    ListView_DeleteAllItems(app.songs_view);

    int row = 0;
    for (size_t index = 0; index < app.songs.count; ++index) {
        const PnSongPath *song = &app.songs.items[index];
        if (query[0] && !pn_wcs_contains_i(song->title, query) &&
            !pn_wcs_contains_i(song->artist, query) &&
            !pn_wcs_contains_i(song->category, query) &&
            !pn_wcs_contains_i(song->aliases, query)) continue;

        wchar_t number[32];
        _snwprintf_s(number, 32, _TRUNCATE, L"%zu", index + 1);
        LVITEMW item;
        memset(&item, 0, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = row;
        item.pszText = number;
        item.lParam = (LPARAM)index;
        int inserted = ListView_InsertItem(app.songs_view, &item);
        if (inserted >= 0) {
            ListView_SetItemText(app.songs_view, inserted, 1,
                                 (wchar_t *)song->artist);
            ListView_SetItemText(app.songs_view, inserted, 2,
                                 (wchar_t *)song->category);
            ListView_SetItemText(app.songs_view, inserted, 3,
                                 (wchar_t *)song->title);
            ++row;
        }
    }

    if (row > 0) {
        ListView_SetItemState(app.songs_view, 0,
                              LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(app.songs_view, 0, FALSE);
    }
    if (!app.playing) {
        wchar_t status[128];
        _snwprintf_s(status, 128, _TRUNCATE,
                     PN_TEXT(L"%d곡 표시 / 전체 %zu곡", L"%d shown / %zu songs"), row, app.songs.count);
        pn_gui_set_status(status);
        if (!app.songs.count)
            pn_gui_set_status(PN_TEXT(
                L"songs 폴더에 MIDI/VPS 파일을 넣은 뒤 새로고침을 눌러 주세요.",
                L"Add MIDI/VPS files to the songs folder, then click Refresh."));
        EnableWindow(app.play, row > 0);
    }
}

static void pn_gui_move_selection(int direction) {
    int count = ListView_GetItemCount(app.songs_view);
    if (count <= 0) return;
    int row = ListView_GetNextItem(app.songs_view, -1, LVNI_SELECTED);
    if (row < 0) row = direction >= 0 ? 0 : count - 1;
    else row = (row + direction + count) % count;
    ListView_SetItemState(app.songs_view, -1, 0,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(app.songs_view, row,
                          LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(app.songs_view, row, FALSE);
    SetFocus(app.songs_view);
}

static void pn_gui_refresh_songs(void) {
    if (app.playing) return;
    PnSongList refreshed;
    wchar_t error[PN_ERROR_CAP] = L"";
    if (!pn_discover_songs(app.songs_dir, &refreshed, error, PN_ERROR_CAP)) {
        MessageBoxW(app.window,
                    error[0] ? error : PN_TEXT(L"곡 목록을 새로고침하지 못했습니다.", L"Could not refresh the song library."),
                    PN_GUI_TITLE, MB_OK | MB_ICONERROR);
        return;
    }
    pn_song_list_free(&app.songs);
    app.songs = refreshed;
    pn_gui_populate_songs();
}

static void pn_gui_resize(int width, int height) {
    const int margin = 18;
    const int search_height = 34;
    const int controls_height = 132;
    int list_top = margin + search_height + 12;
    int list_height = height - list_top - controls_height - margin;
    if (list_height < 120) list_height = 120;

    MoveWindow(app.search, margin, margin,
               width - margin * 2, search_height, TRUE);
    MoveWindow(app.songs_view, margin, list_top,
               width - margin * 2, list_height, TRUE);

    int controls_top = list_top + list_height + 10;
    MoveWindow(app.mode_roblox, margin, controls_top, 96, 28, TRUE);
    MoveWindow(app.mode_vrchat, margin + 102, controls_top, 112, 28, TRUE);
    MoveWindow(app.previous, margin + 230, controls_top, 72, 30, TRUE);
    MoveWindow(app.next, margin + 308, controls_top, 72, 30, TRUE);
    MoveWindow(app.refresh, margin + 386, controls_top, 92, 30, TRUE);
    MoveWindow(app.play, width - margin - 190, controls_top, 88, 30, TRUE);
    MoveWindow(app.stop, width - margin - 94, controls_top, 88, 30, TRUE);

    int options_top = controls_top + 36;
    MoveWindow(app.speed_label, margin, options_top, 66, 28, TRUE);
    MoveWindow(app.speed, margin + 68, options_top, 92, 200, TRUE);
    MoveWindow(app.auto_next, margin + 178, options_top, 132, 28, TRUE);
    MoveWindow(app.human_label, margin + 320, options_top, 70, 28, TRUE);
    MoveWindow(app.human, margin + 392, options_top, 180, 180, TRUE);

    int progress_top = controls_top + 70;
    MoveWindow(app.progress, margin, progress_top,
               width - margin * 2 - 145, 22, TRUE);
    MoveWindow(app.time, width - margin - 137, progress_top - 2,
               137, 26, TRUE);
    MoveWindow(app.status, margin, progress_top + 28,
               width - margin * 2, 24, TRUE);

    int number_width = 58;
    int artist_width = width < 900 ? 150 : 190;
    int category_width = width < 900 ? 110 : 150;
    int title_width = width - margin * 2 - number_width - artist_width - category_width - 24;
    if (title_width < 220) title_width = 220;
    ListView_SetColumnWidth(app.songs_view, 0, number_width);
    ListView_SetColumnWidth(app.songs_view, 1, artist_width);
    ListView_SetColumnWidth(app.songs_view, 2, category_width);
    ListView_SetColumnWidth(app.songs_view, 3, title_width);
}

static void pn_gui_progress_callback(void *user, double elapsed, double total) {
    PnGuiApp *target = (PnGuiApp *)user;
    if (!target || !target->window || target->closing) return;
    unsigned elapsed_ms = (unsigned)(elapsed * 1000.0 + 0.5);
    unsigned total_ms = (unsigned)(total * 1000.0 + 0.5);
    PostMessageW(target->window, WM_PN_PROGRESS,
                 (WPARAM)elapsed_ms, (LPARAM)total_ms);
}

static void pn_gui_pause_callback(void *user, bool paused) {
    PnGuiApp *target = (PnGuiApp *)user;
    if (!target || !target->window || target->closing) return;
    PostMessageW(target->window, WM_PN_PAUSED, (WPARAM)paused, 0);
}

static DWORD WINAPI pn_gui_playback_thread(LPVOID parameter) {
    PnPlaybackJob *job = (PnPlaybackJob *)parameter;
    PnGuiApp *target = job->app;
    PnPlaybackResult *result = (PnPlaybackResult *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PnPlaybackResult)
    );
    if (!result) {
        HeapFree(GetProcessHeap(), 0, job);
        if (!target->closing && target->window)
            PostMessageW(target->window, WM_PN_FINISHED, 0, 0);
        return 0;
    }

    PnOptions options;
    pn_gui_default_options(&options, job->midi);
    options.speed = job->speed;
    options.humanize = job->humanize;
    options.human_seed = job->human_seed;
    options.start_counter = job->start_counter;
    options.stop_event = target->stop_event;
    options.progress_callback = pn_gui_progress_callback;
    options.pause_callback = pn_gui_pause_callback;
    options.progress_user = target;
    pn_copy_wide(options.songs_dir, PN_PATH_CAP, target->songs_dir);

    PnSong song;
    const PnSongPath *entry = &target->songs.items[job->song_index];
    if (!pn_load_song(entry->path, &song, result->error, PN_ERROR_CAP)) {
        result->success = false;
    } else {
        if (!pn_apply_options(&song, &options, result->error, PN_ERROR_CAP)) {
            result->success = false;
        } else {
            result->success = job->midi
                ? pn_play_midi(&song, &options, result->error, PN_ERROR_CAP)
                : pn_play_roblox(&song, &options, result->error, PN_ERROR_CAP);
        }
        pn_song_free(&song);
    }
    result->stopped = target->stop_event &&
        WaitForSingleObject(target->stop_event, 0) == WAIT_OBJECT_0;
    HeapFree(GetProcessHeap(), 0, job);

    if (!target->closing && target->window) {
        PostMessageW(target->window, WM_PN_FINISHED, 0, (LPARAM)result);
    } else {
        HeapFree(GetProcessHeap(), 0, result);
    }
    return 0;
}

static void pn_gui_start_playback(void) {
    if (app.playing) return;
    size_t selected = pn_gui_selected_song();
    if (selected == SIZE_MAX || selected >= app.songs.count) {
        MessageBoxW(app.window, PN_TEXT(L"연주할 곡을 선택해 주세요.", L"Please select a song to play."),
                    PN_GUI_TITLE, MB_OK | MB_ICONINFORMATION);
        return;
    }

    bool midi = SendMessageW(app.mode_vrchat, BM_GETCHECK, 0, 0) == BST_CHECKED;
    LARGE_INTEGER requested_at;
    QueryPerformanceCounter(&requested_at);
    wchar_t error[PN_ERROR_CAP] = L"";
    if (!midi && !pn_focus_window(L"Roblox", error, PN_ERROR_CAP)) {
        MessageBoxW(app.window, error, PN_TEXT(L"Roblox 창을 찾을 수 없습니다", L"Roblox window not found"),
                    MB_OK | MB_ICONWARNING);
        return;
    }

    ResetEvent(app.stop_event);
    SendMessageW(app.progress, PBM_SETPOS, 0, 0);
    SetWindowTextW(app.time, L"00:00 / 00:00");
    app.playing = true;
    pn_gui_set_controls_enabled(false);

    wchar_t status[PN_TITLE_CAP + 96];
    _snwprintf_s(status, PN_TITLE_CAP + 96, _TRUNCATE,
                 midi ? PN_TEXT(L"재생 중: %ls", L"Playing: %ls") : PN_TEXT(L"재생 중 (F8: 잠시 멈춤/이어 연주): %ls", L"Playing (F8: pause/resume): %ls"), app.songs.items[selected].title);
    pn_gui_set_status(status);

    PnPlaybackJob *job = (PnPlaybackJob *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PnPlaybackJob)
    );
    if (!job) {
        app.playing = false;
        pn_gui_set_controls_enabled(true);
        MessageBoxW(app.window, PN_TEXT(L"재생 작업을 만들 메모리가 부족합니다.", L"Not enough memory to start playback."),
                    PN_GUI_TITLE, MB_OK | MB_ICONERROR);
        return;
    }
    job->app = &app;
    job->song_index = selected;
    job->midi = midi;
    job->speed = pn_gui_selected_speed();
    job->humanize = (int)SendMessageW(app.human, CB_GETCURSEL, 0, 0);
    if (job->humanize < 0 || job->humanize > 3) job->humanize = 2;
    job->human_seed = GetTickCount() ^ (uint32_t)selected;
    job->start_counter = requested_at.QuadPart;
    app.worker = CreateThread(NULL, 0, pn_gui_playback_thread, job, 0, NULL);
    if (!app.worker) {
        HeapFree(GetProcessHeap(), 0, job);
        app.playing = false;
        pn_gui_set_controls_enabled(true);
        MessageBoxW(app.window, PN_TEXT(L"재생 스레드를 시작하지 못했습니다.", L"Could not start the playback thread."),
                    PN_GUI_TITLE, MB_OK | MB_ICONERROR);
    }
}

static void pn_gui_stop_playback(void) {
    if (!app.playing) return;
    SetEvent(app.stop_event);
    pn_gui_set_status(PN_TEXT(L"정지 중... 눌린 건반과 페달을 해제하고 있습니다.", L"Stopping... releasing held keys and pedal."));
    EnableWindow(app.stop, FALSE);
}

static HWND pn_gui_create_control(const wchar_t *class_name,
                                  const wchar_t *text, DWORD style, int id) {
    HWND control = CreateWindowExW(
        0, class_name, text, WS_CHILD | WS_VISIBLE | style,
        0, 0, 0, 0, app.window, (HMENU)(INT_PTR)id,
        app.instance, NULL
    );
    if (control) pn_gui_apply_font(control);
    return control;
}

static bool pn_gui_create_controls(void) {
    app.search = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_EDITW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 0, 0, app.window, (HMENU)(INT_PTR)IDC_SEARCH,
        app.instance, NULL
    );
    app.songs_view = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
        LVS_SHOWSELALWAYS,
        0, 0, 0, 0, app.window, (HMENU)(INT_PTR)IDC_SONGS,
        app.instance, NULL
    );
    app.mode_roblox = pn_gui_create_control(
        WC_BUTTONW, L"Roblox", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
        IDC_MODE_ROBLOX
    );
    app.mode_vrchat = pn_gui_create_control(
        WC_BUTTONW, L"VRChat MIDI", BS_AUTORADIOBUTTON | WS_TABSTOP,
        IDC_MODE_VRCHAT
    );
    app.play = pn_gui_create_control(
        WC_BUTTONW, PN_TEXT(L"재생", L"Play"), BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_PLAY
    );
    app.stop = pn_gui_create_control(
        WC_BUTTONW, PN_TEXT(L"정지", L"Stop"), BS_PUSHBUTTON | WS_TABSTOP, IDC_STOP
    );
    app.previous = pn_gui_create_control(
        WC_BUTTONW, PN_TEXT(L"이전 곡", L"Previous"), BS_PUSHBUTTON | WS_TABSTOP, IDC_PREVIOUS
    );
    app.next = pn_gui_create_control(
        WC_BUTTONW, PN_TEXT(L"다음 곡", L"Next"), BS_PUSHBUTTON | WS_TABSTOP, IDC_NEXT
    );
    app.refresh = pn_gui_create_control(
        WC_BUTTONW, PN_TEXT(L"새로고침", L"Refresh"), BS_PUSHBUTTON | WS_TABSTOP, IDC_REFRESH
    );
    app.speed_label = pn_gui_create_control(
        WC_STATICW, PN_TEXT(L"재생 속도", L"Speed"), SS_LEFT | SS_CENTERIMAGE, IDC_SPEED_LABEL
    );
    app.speed = pn_gui_create_control(
        WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
        IDC_SPEED
    );
    app.auto_next = pn_gui_create_control(
        WC_BUTTONW, PN_TEXT(L"연속 재생", L"Auto-next"), BS_AUTOCHECKBOX | WS_TABSTOP, IDC_AUTO_NEXT
    );
    app.human_label = pn_gui_create_control(
        WC_STATICW, PN_TEXT(L"연주 표현", L"Style"), SS_LEFT | SS_CENTERIMAGE, IDC_HUMAN_LABEL
    );
    app.human = pn_gui_create_control(
        WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_HUMAN
    );
    app.progress = pn_gui_create_control(
        PROGRESS_CLASSW, L"", PBS_SMOOTH, IDC_PROGRESS
    );
    app.time = pn_gui_create_control(
        WC_STATICW, L"00:00 / 00:00", SS_RIGHT | SS_CENTERIMAGE, IDC_TIME
    );
    app.status = pn_gui_create_control(
        WC_STATICW, PN_TEXT(L"준비됨", L"Ready"), SS_LEFT | SS_CENTERIMAGE, IDC_STATUS
    );

    if (!app.search || !app.songs_view || !app.mode_roblox ||
        !app.mode_vrchat || !app.play || !app.stop || !app.progress ||
        !app.time || !app.status || !app.previous || !app.next ||
        !app.refresh || !app.speed_label || !app.speed ||
        !app.auto_next || !app.human_label || !app.human) return false;

    pn_gui_apply_font(app.search);
    pn_gui_apply_font(app.songs_view);
    SendMessageW(app.search, EM_SETCUEBANNER, TRUE,
                 (LPARAM)PN_TEXT(L"곡 제목 또는 분류 검색", L"Search title, artist, category or alias"));
    SendMessageW(app.mode_roblox, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(app.speed, CB_ADDSTRING, 0, (LPARAM)L"75%");
    SendMessageW(app.speed, CB_ADDSTRING, 0, (LPARAM)L"80%");
    SendMessageW(app.speed, CB_ADDSTRING, 0, (LPARAM)L"90%");
    SendMessageW(app.speed, CB_ADDSTRING, 0, (LPARAM)L"100%");
    SendMessageW(app.speed, CB_ADDSTRING, 0, (LPARAM)L"110%");
    SendMessageW(app.speed, CB_ADDSTRING, 0, (LPARAM)L"125%");
    SendMessageW(app.speed, CB_SETCURSEL, 3, 0);
    SendMessageW(app.human, CB_ADDSTRING, 0, (LPARAM)PN_TEXT(L"원본 (보정 끄기)", L"Original (off)"));
    SendMessageW(app.human, CB_ADDSTRING, 0, (LPARAM)PN_TEXT(L"은은하게", L"Subtle"));
    SendMessageW(app.human, CB_ADDSTRING, 0, (LPARAM)PN_TEXT(L"자연스럽게", L"Natural"));
    SendMessageW(app.human, CB_ADDSTRING, 0, (LPARAM)PN_TEXT(L"표현 풍부하게", L"Expressive"));
    SendMessageW(app.human, CB_SETCURSEL, 2, 0);
    EnableWindow(app.stop, FALSE);
    SendMessageW(app.progress, PBM_SETRANGE32, 0, 1000);
    ListView_SetExtendedListViewStyle(
        app.songs_view, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES
    );

    LVCOLUMNW column;
    memset(&column, 0, sizeof(column));
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = PN_TEXT(L"번호", L"No.");
    column.cx = 58;
    ListView_InsertColumn(app.songs_view, 0, &column);
    column.pszText = PN_TEXT(L"아티스트 / 작곡가", L"Artist / Composer");
    column.cx = 190;
    ListView_InsertColumn(app.songs_view, 1, &column);
    column.pszText = PN_TEXT(L"분류", L"Category");
    column.cx = 150;
    ListView_InsertColumn(app.songs_view, 2, &column);
    column.pszText = PN_TEXT(L"곡 제목", L"Song title");
    column.cx = 560;
    ListView_InsertColumn(app.songs_view, 3, &column);
    pn_gui_populate_songs();
    return true;
}

static LRESULT CALLBACK pn_gui_window_proc(HWND window, UINT message,
                                            WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE:
            app.window = window;
            if (!pn_gui_create_controls()) return -1;
            return 0;

        case WM_SIZE:
            pn_gui_resize(LOWORD(lparam), HIWORD(lparam));
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO *limits = (MINMAXINFO *)lparam;
            limits->ptMinTrackSize.x = 980;
            limits->ptMinTrackSize.y = 700;
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case IDC_SEARCH:
                    if (HIWORD(wparam) == EN_CHANGE && !app.playing)
                        pn_gui_populate_songs();
                    return 0;
                case IDC_PLAY:
                    if (HIWORD(wparam) == BN_CLICKED) pn_gui_start_playback();
                    return 0;
                case IDC_STOP:
                    if (HIWORD(wparam) == BN_CLICKED) pn_gui_stop_playback();
                    return 0;
                case IDC_PREVIOUS:
                    if (HIWORD(wparam) == BN_CLICKED) pn_gui_move_selection(-1);
                    return 0;
                case IDC_NEXT:
                    if (HIWORD(wparam) == BN_CLICKED) pn_gui_move_selection(1);
                    return 0;
                case IDC_REFRESH:
                    if (HIWORD(wparam) == BN_CLICKED) pn_gui_refresh_songs();
                    return 0;
            }
            break;

        case WM_KEYDOWN:
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wparam == L'F') {
                SetFocus(app.search);
                SendMessageW(app.search, EM_SETSEL, 0, -1);
                return 0;
            }
            if (wparam == VK_F6 && !app.playing) {
                pn_gui_start_playback();
                return 0;
            }
            if (wparam == VK_F7 && app.playing) {
                pn_gui_stop_playback();
                return 0;
            }
            if (wparam == VK_F8 && !app.playing) {
                pn_gui_move_selection(1);
                return 0;
            }
            break;

        case WM_NOTIFY: {
            NMHDR *header = (NMHDR *)lparam;
            if (header->idFrom == IDC_SONGS && header->code == NM_DBLCLK) {
                pn_gui_start_playback();
                return 0;
            }
            break;
        }

        case WM_PN_PAUSED:
            if (app.playing && !app.closing &&
                WaitForSingleObject(app.stop_event, 0) != WAIT_OBJECT_0) {
                pn_gui_set_status(wparam
                    ? PN_TEXT(L"일시정지: Roblox로 돌아오세요. F8로 멈췄다면 F8을 다시 눌러 주세요.", L"Paused: return to Roblox. If paused with F8, press F8 again.")
                    : PN_TEXT(L"재생 중: 같은 위치에서 이어갑니다. F8: 잠시 멈춤/이어 연주", L"Resumed from the same position. F8: pause/resume"));
            }
            return 0;

        case WM_PN_PROGRESS: {
            unsigned elapsed_ms = (unsigned)wparam;
            unsigned total_ms = (unsigned)lparam;
            int position = total_ms ? (int)((uint64_t)elapsed_ms * 1000 / total_ms) : 0;
            if (position < 0) position = 0;
            if (position > 1000) position = 1000;
            SendMessageW(app.progress, PBM_SETPOS, (WPARAM)position, 0);
            wchar_t text[64];
            pn_gui_format_time(text, 64, elapsed_ms, total_ms);
            SetWindowTextW(app.time, text);
            return 0;
        }

        case WM_PN_FINISHED: {
            PnPlaybackResult *result = (PnPlaybackResult *)lparam;
            bool play_next = false;
            if (app.worker) {
                CloseHandle(app.worker);
                app.worker = NULL;
            }
            app.playing = false;
            pn_gui_set_controls_enabled(true);
            if (result) {
                if (result->success) {
                    pn_gui_set_status(PN_TEXT(L"재생이 완료되었습니다.", L"Playback finished."));
                    play_next = SendMessageW(app.auto_next, BM_GETCHECK, 0, 0) ==
                        BST_CHECKED;
                } else if (result->stopped) {
                    pn_gui_set_status(PN_TEXT(L"재생을 정지했습니다.", L"Playback stopped."));
                } else {
                    pn_gui_set_status(PN_TEXT(L"재생하지 못했습니다.", L"Playback failed."));
                    MessageBoxW(app.window,
                                result->error[0] ? result->error : PN_TEXT(L"알 수 없는 오류입니다.", L"An unknown error occurred."),
                                PN_TEXT(L"재생 오류", L"Playback error"), MB_OK | MB_ICONERROR);
                }
                HeapFree(GetProcessHeap(), 0, result);
            } else {
                pn_gui_set_status(PN_TEXT(L"메모리가 부족하여 재생을 시작하지 못했습니다.",
                                         L"Not enough memory to start playback."));
            }
            if (play_next && !app.closing) {
                pn_gui_move_selection(1);
                pn_gui_start_playback();
            }
            return 0;
        }

        case WM_CLOSE:
            if (app.playing) {
                SetEvent(app.stop_event);
                pn_gui_set_status(PN_TEXT(L"종료 준비 중...", L"Closing..."));
                app.closing = true;
                if (app.worker) WaitForSingleObject(app.worker, 3000);
            }
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            app.window = NULL;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous,
                    PWSTR command_line, int show_command) {
    (void)previous;
    (void)show_command;
    bool smoke_test = command_line &&
        pn_wcs_contains_i(command_line, L"--smoke-test");
    memset(&app, 0, sizeof(app));
    app.instance = instance;
    SetProcessDPIAware();

    INITCOMMONCONTROLSEX controls = {
        sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS
    };
    InitCommonControlsEx(&controls);

    if (!pn_gui_find_songs_dir(app.songs_dir, PN_PATH_CAP)) {
        MessageBoxW(NULL,
                    PN_TEXT(L"songs 폴더를 찾지 못했습니다. 프로그램을 songs 폴더 옆에 두어 주세요.", L"The songs folder was not found. Keep it beside the executable."),
                    PN_GUI_TITLE, MB_OK | MB_ICONERROR);
        return 2;
    }
    wchar_t error[PN_ERROR_CAP] = L"";
    if (!pn_discover_songs(app.songs_dir, &app.songs, error, PN_ERROR_CAP)) {
        MessageBoxW(NULL, error[0] ? error : PN_TEXT(L"곡 목록을 읽지 못했습니다.", L"Could not read the song library."),
                    PN_GUI_TITLE, MB_OK | MB_ICONERROR);
        return 2;
    }
    app.stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!app.stop_event) {
        pn_song_list_free(&app.songs);
        MessageBoxW(NULL, PN_TEXT(L"정지 이벤트를 만들지 못했습니다.", L"Could not create the stop event."),
                    PN_GUI_TITLE, MB_OK | MB_ICONERROR);
        return 2;
    }

    NONCLIENTMETRICSW metrics;
    memset(&metrics, 0, sizeof(metrics));
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                              &metrics, 0)) {
        app.font = CreateFontIndirectW(&metrics.lfMessageFont);
    }
    if (!app.font) app.font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    WNDCLASSEXW window_class;
    memset(&window_class, 0, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = pn_gui_window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    window_class.hIconSm = window_class.hIcon;
    window_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    window_class.lpszClassName = PN_GUI_CLASS;
    if (!RegisterClassExW(&window_class)) {
        CloseHandle(app.stop_event);
        pn_song_list_free(&app.songs);
        return 2;
    }

    HWND window = CreateWindowExW(
        0, PN_GUI_CLASS, PN_GUI_TITLE,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 980, 700,
        NULL, NULL, instance, NULL
    );
    if (!window) {
        CloseHandle(app.stop_event);
        pn_song_list_free(&app.songs);
        if (app.font && app.font != GetStockObject(DEFAULT_GUI_FONT))
            DeleteObject(app.font);
        return 2;
    }

    if (smoke_test) {
        PnOptions smoke_options;
        pn_gui_default_options(&smoke_options, false);
        bool timing_ok = smoke_options.start_delay == 1.2;
        pn_gui_default_options(&smoke_options, true);
        timing_ok = timing_ok && smoke_options.start_delay == 1.2;
        bool controls_ok = timing_ok && app.search && app.songs_view &&
            Header_GetItemCount(ListView_GetHeader(app.songs_view)) == 4 && app.mode_roblox &&
            app.mode_vrchat && app.play && app.stop && app.progress &&
            app.time && app.status && app.previous && app.next && app.refresh &&
            app.speed && app.auto_next && app.human &&
            SendMessageW(app.human, CB_GETCOUNT, 0, 0) == 4 &&
            SendMessageW(app.human, CB_GETCURSEL, 0, 0) == 2;
        int initial_count = ListView_GetItemCount(app.songs_view);
        SetWindowTextW(app.search, L"__PN_SMOKE_NO_MATCH_8C45D7__");
        int filtered_count = ListView_GetItemCount(app.songs_view);
        bool passed = controls_ok && initial_count == (int)app.songs.count &&
                      filtered_count == 0 && !IsWindowEnabled(app.play);
        SetWindowTextW(app.search, L"");
        passed = passed && ListView_GetItemCount(app.songs_view) == initial_count &&
            (IsWindowEnabled(app.play) != FALSE) == (initial_count > 0);
        wchar_t play_label[64];
        GetWindowTextW(app.play, play_label, 64);
        passed = passed && wcscmp(play_label, PN_TEXT(L"재생", L"Play")) == 0;
        wchar_t pause_status[256];
        app.playing = true;
        SendMessageW(window, WM_PN_PAUSED, TRUE, 0);
        GetWindowTextW(app.status, pause_status, 256);
        passed = passed && pn_wcs_contains_i(pause_status, PN_TEXT(L"일시정지", L"Paused"));
        SendMessageW(window, WM_PN_PAUSED, FALSE, 0);
        GetWindowTextW(app.status, pause_status, 256);
        passed = passed && pn_wcs_contains_i(pause_status, PN_TEXT(L"이어갑니다", L"Resumed"));
        app.playing = false;
        DestroyWindow(window);
        CloseHandle(app.stop_event);
        pn_song_list_free(&app.songs);
        if (app.font && app.font != GetStockObject(DEFAULT_GUI_FONT))
            DeleteObject(app.font);
        return passed ? 0 : 3;
    }

    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    UpdateWindow(window);

    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    if (app.worker) CloseHandle(app.worker);
    CloseHandle(app.stop_event);
    pn_song_list_free(&app.songs);
    if (app.font && app.font != GetStockObject(DEFAULT_GUI_FONT))
        DeleteObject(app.font);
    return (int)message.wParam;
}
