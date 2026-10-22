#include "piano_native.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct PnClock {
    LARGE_INTEGER frequency;
    LARGE_INTEGER started;
    HANDLE timer;
    bool *held;
    uint32_t *held_generation;
    LONGLONG key_changed[256];
    bool key_known[256];
    bool manual_paused;
    bool pause_hotkey_down;
} PnClock;

static void pn_release_all_keys(bool *held);

typedef struct PnWindowSearch {
    const wchar_t *needle;
    HWND found;
    wchar_t title[PN_TITLE_CAP];
} PnWindowSearch;

typedef struct PnKeyNote {
    double time;
    wchar_t character;
    int slot;
    UINT vk;
    UINT scan;
    bool shifted;
    uint32_t generation;
    double duration;
} PnKeyNote;

typedef struct PnKeyEvent {
    double time;
    bool down;
    wchar_t character;
    UINT vk;
    UINT scan;
    bool shifted;
    uint32_t generation;
} PnKeyEvent;

/* White/black notes can share a Windows key. Protect both input edges rather
   than clamping the release below the next attack and then undoing that clamp
   with a minimum hold. This is an input transport limit, not quantization. */
#define PN_KEY_MIN_HOLD 0.008
#define PN_KEY_MIN_GAP 0.004
#define PN_KEY_MAX_DELAY 0.032

static bool pn_aborted(const PnOptions *options) {
    if (options && options->stop_event &&
        WaitForSingleObject(options->stop_event, 0) == WAIT_OBJECT_0) return true;
    return (GetAsyncKeyState(VK_ESCAPE) & 0x8000) ||
           (GetAsyncKeyState(VK_F7) & 0x8000);
}

static double pn_clock_now(const PnClock *clock) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - clock->started.QuadPart) /
           (double)clock->frequency.QuadPart;
}

static bool pn_clock_start(PnClock *clock, const PnOptions *options, wchar_t *error, size_t cap) {
    memset(clock, 0, sizeof(*clock));
    if (!QueryPerformanceFrequency(&clock->frequency) ||
        !QueryPerformanceCounter(&clock->started)) {
        pn_set_error(error, cap, L"QueryPerformanceCounter is unavailable.");
        return false;
    }
    if (options && options->start_counter > 0 && options->start_counter <= clock->started.QuadPart)
        clock->started.QuadPart = options->start_counter;
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    clock->timer = CreateWaitableTimerExW(
        NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS
    );
    if (!clock->timer) clock->timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    timeBeginPeriod(1);
    return true;
}

static void pn_clock_close(PnClock *clock) {
    if (clock->timer) CloseHandle(clock->timer);
    timeEndPeriod(1);
}

static void pn_show_progress(double elapsed, double total,
                             const PnOptions *options,
                             double *last_progress, bool force) {
    if (total <= 0.0 || !last_progress) return;
    if (elapsed < 0.0) elapsed = 0.0;
    if (elapsed > total) elapsed = total;
    if (!force && *last_progress >= 0.0 && elapsed - *last_progress < 0.10) return;

    if (options && options->progress_callback) {
        options->progress_callback(options->progress_user, elapsed, total);
    }

    if (options && options->quiet) {
        *last_progress = elapsed;
        return;
    }

    const int width = 24;
    double ratio = elapsed / total;
    int filled = (int)(ratio * width + 0.5);
    if (filled < 0) filled = 0;
    if (filled > width) filled = width;
    int elapsed_seconds = (int)elapsed;
    int total_seconds = (int)(total + 0.5);

    wprintf(PN_TEXT(L"\r  진행 [", L"\r  Progress ["));
    for (int index = 0; index < width; ++index) {
        putwchar(index < filled ? L'#' : L'-');
    }
    wprintf(L"] %02d:%02d / %02d:%02d  %5.1f%%",
            elapsed_seconds / 60, elapsed_seconds % 60,
            total_seconds / 60, total_seconds % 60,
            ratio * 100.0);
    fflush(stdout);
    *last_progress = elapsed;
}

static void pn_poll_pause_hotkey(PnClock *clock, HWND target) {
    bool down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (down && !clock->pause_hotkey_down && GetForegroundWindow() == target)
        clock->manual_paused = !clock->manual_paused;
    clock->pause_hotkey_down = down;
}

static bool pn_pause_for_focus(PnClock *clock, HWND target, double total_duration,
                               const PnOptions *options, double *last_progress,
                               wchar_t *error, size_t cap) {
    LARGE_INTEGER pause_started, resumed;
    QueryPerformanceCounter(&pause_started);
    pn_show_progress(pn_clock_now(clock), total_duration, options, last_progress, true);
    /* Only key-up cleanup is sent on focus loss; never press keys in another app.
       Do not re-strike held notes on resume: that would sound the same note twice. */
    if (clock->held) {
        bool released[256];
        memcpy(released, clock->held, sizeof(released));
        for (UINT vk = 0; vk < 256; ++vk) {
            if (!clock->held[vk]) continue;
            pn_release_all_keys(clock->held);
            break;
        }
        LARGE_INTEGER cleaned;
        QueryPerformanceCounter(&cleaned);
        for (UINT vk = 0; vk < 256; ++vk) if (released[vk]) {
            clock->key_changed[vk] = cleaned.QuadPart;
            clock->key_known[vk] = true;
        }
    }
    if (clock->held_generation) memset(clock->held_generation, 0, 256 * sizeof(uint32_t));
    if (options && options->pause_callback)
        options->pause_callback(options->progress_user, true);
    if (!options || !options->quiet)
        wprintf(PN_TEXT(L"\n  일시정지: Roblox로 돌아오세요. F8로 멈춘 경우 F8을 다시 누르면 이어 연주합니다.\n",
                        L"\n  Paused: return to Roblox. If paused with F8, press F8 again.\n"));

    bool ok = true;
    for (;;) {
        if (pn_aborted(options)) {
            pn_set_error(error, cap, L"Playback stopped while paused.");
            ok = false; break;
        }
        if (!IsWindow(target)) {
            pn_set_error(error, cap, L"Playback stopped because the target window closed.");
            ok = false; break;
        }
        pn_poll_pause_hotkey(clock, target);
        if (GetForegroundWindow() == target && !clock->manual_paused) break;
        if (options && options->stop_event)
            WaitForSingleObject(options->stop_event, 10);
        else Sleep(10);
    }
    QueryPerformanceCounter(&resumed);
    /* Move the clock origin by the pause duration. Pending events retain their
       deadlines, so no notes are skipped or sent in a catch-up burst. */
    clock->started.QuadPart += resumed.QuadPart - pause_started.QuadPart;
    if (ok && options && options->pause_callback)
        options->pause_callback(options->progress_user, false);
    if (ok && (!options || !options->quiet))
        wprintf(PN_TEXT(L"  연주를 이어갑니다.\n", L"  Playback resumed.\n"));
    return ok;
}

static bool pn_wait_until(PnClock *clock, double deadline, HWND required_window,
                          double total_duration, const PnOptions *options,
                          double *last_progress,
                          wchar_t *error, size_t cap) {
    for (;;) {
        double elapsed = pn_clock_now(clock);
        pn_show_progress(elapsed, total_duration, options, last_progress, false);
        if (pn_aborted(options)) {
            if (options && options->stop_event &&
                WaitForSingleObject(options->stop_event, 0) == WAIT_OBJECT_0) {
                pn_set_error(error, cap, L"Playback stopped.");
            } else {
                pn_set_error(error, cap, L"Playback stopped by F7 or ESC.");
            }
            return false;
        }
        if (required_window) pn_poll_pause_hotkey(clock, required_window);
        if (required_window && (clock->manual_paused || GetForegroundWindow() != required_window)) {
            if (!pn_pause_for_focus(clock, required_window, total_duration,
                                     options, last_progress, error, cap)) return false;
            continue;
        }
        double remaining = deadline - elapsed;
        if (remaining <= 0.0) return true;
        if (remaining > 0.0020) {
            double sleep_part = remaining - 0.0010;
            if (sleep_part > 0.020) sleep_part = 0.020;
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(sleep_part * 10000000.0);
            if (clock->timer && SetWaitableTimer(clock->timer, &due, 0, NULL, NULL, FALSE)) {
                WaitForSingleObject(clock->timer, INFINITE);
            } else {
                DWORD millis = (DWORD)(sleep_part * 1000.0);
                Sleep(millis ? millis : 1);
            }
        } else {
            while (pn_clock_now(clock) < deadline) YieldProcessor();
            return true;
        }
    }
}

static BOOL CALLBACK pn_find_window_callback(HWND hwnd, LPARAM parameter) {
    PnWindowSearch *search = (PnWindowSearch *)parameter;
    if (!IsWindowVisible(hwnd)) return TRUE;
    int length = GetWindowTextLengthW(hwnd);
    if (length <= 0 || length >= PN_TITLE_CAP) return TRUE;
    wchar_t title[PN_TITLE_CAP];
    if (!GetWindowTextW(hwnd, title, PN_TITLE_CAP)) return TRUE;
    if (!pn_wcs_contains_i(title, search->needle)) return TRUE;
    search->found = hwnd;
    pn_copy_wide(search->title, PN_TITLE_CAP, title);
    return FALSE;
}

static HWND pn_find_window(const wchar_t *fragment, wchar_t *title) {
    PnWindowSearch search = {fragment, NULL, L""};
    EnumWindows(pn_find_window_callback, (LPARAM)&search);
    if (title) pn_copy_wide(title, PN_TITLE_CAP, search.title);
    return search.found;
}

bool pn_focus_window(const wchar_t *fragment, wchar_t *error, size_t cap) {
    wchar_t title[PN_TITLE_CAP] = L"";
    HWND target = pn_find_window(fragment, title);
    if (!target) {
        pn_set_error(error, cap, L"No visible window matches '%ls'.", fragment);
        return false;
    }
    if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
    if (!SetForegroundWindow(target)) {
        pn_set_error(error, cap, L"Could not bring '%ls' to the foreground.", title);
        return false;
    }
    return true;
}

static bool pn_send_short(HMIDIOUT handle, uint8_t status,
                          uint8_t data1, uint8_t data2,
                          wchar_t *error, size_t cap) {
    DWORD message = status | ((DWORD)data1 << 8) | ((DWORD)data2 << 16);
    MMRESULT result = midiOutShortMsg(handle, message);
    if (result != MMSYSERR_NOERROR) {
        wchar_t detail[256] = L"";
        midiOutGetErrorTextW(result, detail, 256);
        pn_set_error(error, cap, L"MIDI output failed: %ls", detail);
        return false;
    }
    return true;
}

void pn_midi_panic(HMIDIOUT handle) {
    if (!handle) return;
    for (int note = 0; note < 128; ++note) {
        midiOutShortMsg(handle, 0x80u | ((DWORD)note << 8));
    }
    midiOutShortMsg(handle, 0xB0u | (64u << 8));
    midiOutShortMsg(handle, 0xB0u | (66u << 8));
    midiOutShortMsg(handle, 0xB0u | (67u << 8));
    midiOutShortMsg(handle, 0xB0u | (123u << 8));
    midiOutReset(handle);
}

static bool pn_open_midi_port(const wchar_t *query, HMIDIOUT *handle,
                              wchar_t *selected, size_t selected_cap,
                              wchar_t *error, size_t cap) {
    UINT count = midiOutGetNumDevs();
    if (count == 0) {
        pn_set_error(error, cap, L"No Windows MIDI output device was found.");
        return false;
    }
    int chosen = -1;
    MIDIOUTCAPSW caps_data;
    for (UINT index = 0; index < count; ++index) {
        if (midiOutGetDevCapsW(index, &caps_data, sizeof(caps_data)) != MMSYSERR_NOERROR)
            continue;
        if (!query[0] || pn_wcs_contains_i(caps_data.szPname, query)) {
            chosen = (int)index;
            pn_copy_wide(selected, selected_cap, caps_data.szPname);
            break;
        }
    }
    if (chosen < 0) {
        pn_set_error(error, cap, L"No MIDI output port matches '%ls'.", query);
        return false;
    }
    MMRESULT result = midiOutOpen(handle, (UINT)chosen, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR) {
        wchar_t detail[256] = L"";
        midiOutGetErrorTextW(result, detail, 256);
        pn_set_error(error, cap, L"Cannot open MIDI port '%ls': %ls", selected, detail);
        return false;
    }
    return true;
}

static bool pn_midi_event(HMIDIOUT output, const PnEvent *event,
                          uint32_t *active, wchar_t *error, size_t cap) {
    if (event->type == PN_NOTE_ON) {
        if (active[event->data1] &&
            !pn_send_short(output, 0x80, event->data1, 0, error, cap)) return false;
        if (!pn_send_short(output, 0x90, event->data1,
                           event->data2 ? event->data2 : 1, error, cap)) return false;
        active[event->data1] = event->note_id;
    } else if (event->type == PN_NOTE_OFF) {
        /* Ignore a late release belonging to an earlier strike of this pitch.
           The old bool gate could release a newer note with a different ID. */
        if (active[event->data1] && active[event->data1] == event->note_id) {
            if (!pn_send_short(output, 0x80, event->data1, 0, error, cap)) return false;
            active[event->data1] = 0;
        }
    } else if (event->type == PN_CONTROL_CHANGE) {
        return pn_send_short(output, 0xB0, event->data1, event->data2, error, cap);
    }
    return true;
}

bool pn_play_midi(const PnSong *song, const PnOptions *options,
                  wchar_t *error, size_t cap) {
    HMIDIOUT output = NULL;
    wchar_t selected[PN_TITLE_CAP] = L"";
    if (!pn_open_midi_port(options->port_query, &output, selected,
                           PN_TITLE_CAP, error, cap)) return false;
    if (!options->quiet) wprintf(L"  MIDI output: %ls\n", selected);

    uint32_t active[128] = {0};
    PnClock clock;
    double last_progress = -1.0;
    bool ok = pn_clock_start(&clock, options, error, cap);
    if (!ok) {
        pn_midi_panic(output);
        midiOutClose(output);
        return false;
    }
    for (size_t index = 0; index < song->event_count && ok; ++index) {
        PnEvent event = song->events[index];
        if (!pn_wait_until(&clock, event.time, NULL, song->duration,
                           options, &last_progress, error, cap)) {
            ok = false;
            break;
        }
        ok = pn_midi_event(output, &event, active, error, cap);
    }
    pn_show_progress(ok ? song->duration : pn_clock_now(&clock),
                     song->duration, options, &last_progress, true);
    if (!options->quiet) wprintf(L"\n");
    pn_midi_panic(output);
    pn_clock_close(&clock);
    midiOutClose(output);
    return ok;
}

static bool pn_note_character(int note, wchar_t *character, int *slot) {
    static const wchar_t white[] = L"1234567890qwertyuiopasdfghjklzxcvbnm";
    static const wchar_t black[] = L"!@$%^*(QWETYIOPSDGHJLZCVB";
    static const bool has_black[] = {true, true, false, true, true, true, false};
    int midi = PN_LOWEST_MIDI;
    int white_index = 0;
    int black_index = 0;
    int current_slot = 0;
    while (white[white_index]) {
        if (midi == note) {
            *character = white[white_index];
            *slot = current_slot;
            return true;
        }
        ++midi;
        ++current_slot;
        if (has_black[white_index % 7] && black[black_index]) {
            if (midi == note) {
                *character = black[black_index];
                *slot = current_slot;
                return true;
            }
            ++midi;
            ++black_index;
            ++current_slot;
        }
        ++white_index;
    }
    return false;
}

static bool pn_key_info(wchar_t character, UINT *vk, UINT *scan, bool *shifted) {
    *shifted = false;
    wchar_t lower = character;
    if (character >= L'A' && character <= L'Z') {
        lower = character + (L'a' - L'A');
        *shifted = true;
    }
    if (lower >= L'a' && lower <= L'z') *vk = (UINT)(L'A' + lower - L'a');
    else if (lower >= L'0' && lower <= L'9') *vk = (UINT)lower;
    else {
        *shifted = true;
        switch (character) {
            case L'!': *vk = L'1'; break;
            case L'@': *vk = L'2'; break;
            case L'$': *vk = L'4'; break;
            case L'%': *vk = L'5'; break;
            case L'^': *vk = L'6'; break;
            case L'*': *vk = L'8'; break;
            case L'(': *vk = L'9'; break;
            default: return false;
        }
    }
    *scan = MapVirtualKeyW(*vk, MAPVK_VK_TO_VSC);
    return *scan != 0;
}

static int pn_key_note_compare(const void *left_ptr, const void *right_ptr) {
    const PnKeyNote *left = (const PnKeyNote *)left_ptr;
    const PnKeyNote *right = (const PnKeyNote *)right_ptr;
    if (left->time < right->time) return -1;
    if (left->time > right->time) return 1;
    if (left->vk < right->vk) return -1;
    if (left->vk > right->vk) return 1;
    if (left->shifted != right->shifted) return left->shifted ? -1 : 1;
    return 0;
}

static int pn_key_event_compare(const void *left_ptr, const void *right_ptr) {
    const PnKeyEvent *left = (const PnKeyEvent *)left_ptr;
    const PnKeyEvent *right = (const PnKeyEvent *)right_ptr;
    if (left->time < right->time) return -1;
    if (left->time > right->time) return 1;
    if (left->down != right->down) return left->down ? 1 : -1;
    if (left->generation < right->generation) return -1;
    if (left->generation > right->generation) return 1;
    return 0;
}

static INPUT pn_keyboard_input(UINT vk, UINT scan, bool up) {
    INPUT input;
    memset(&input, 0, sizeof(input));
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = 0;
    input.ki.wScan = (WORD)scan;
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0);
    (void)vk;
    return input;
}

static bool pn_send_inputs(INPUT *inputs, UINT count,
                           wchar_t *error, size_t cap) {
    if (!count) return true;
    UINT sent = SendInput(count, inputs, sizeof(INPUT));
    if (sent != count) {
        pn_set_error(error, cap,
                     L"SendInput sent %u/%u events (Windows error %lu).",
                     sent, count, GetLastError());
        return false;
    }
    return true;
}

static bool pn_key_transition(const PnKeyEvent *event, bool *held,
                              uint32_t *held_generation,
                              wchar_t *error, size_t cap) {
    UINT shift_scan = MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC);
    if (!event->down && (!held[event->vk] ||
                         held_generation[event->vk] != event->generation)) {
        return true;
    }
    if (event->down && held[event->vk]) {
        INPUT release = pn_keyboard_input(event->vk, event->scan, true);
        if (!pn_send_inputs(&release, 1, error, cap)) return false;
        held[event->vk] = false;
    }
    INPUT inputs[3];
    UINT count = 0;
    if (event->down && event->shifted) {
        inputs[count++] = pn_keyboard_input(VK_LSHIFT, shift_scan, false);
    }
    inputs[count++] = pn_keyboard_input(event->vk, event->scan, !event->down);
    if (event->down && event->shifted) {
        inputs[count++] = pn_keyboard_input(VK_LSHIFT, shift_scan, true);
    }
    /* SendInput can report a partial batch (e.g. Shift-down + note-down but
       not Shift-up). Track a possibly pressed note before sending so the
       caller's error cleanup releases it as well as the modifier. */
    if (event->down) {
        held[event->vk] = true;
        held_generation[event->vk] = event->generation;
    }
    if (!pn_send_inputs(inputs, count, error, cap)) return false;
    held[event->vk] = event->down;
    held_generation[event->vk] = event->down ? event->generation : 0;
    return true;
}

static void pn_release_all_keys(bool *held) {
    INPUT inputs[257];
    UINT count = 0;
    for (UINT vk = 0; vk < 256; ++vk) {
        if (!held[vk]) continue;
        UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        if (scan) inputs[count++] = pn_keyboard_input(vk, scan, true);
        held[vk] = false;
    }
    UINT shift_scan = MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC);
    inputs[count++] = pn_keyboard_input(VK_LSHIFT, shift_scan, true);
    SendInput(count, inputs, sizeof(INPUT));
}

static bool pn_build_key_schedule(const PnSong *song, const PnOptions *options,
                                  PnKeyEvent **events_out, size_t *count_out,
                                  wchar_t *error, size_t cap) {
    size_t maximum = song->event_count;
    PnKeyNote *notes = (PnKeyNote *)calloc(maximum ? maximum : 1, sizeof(PnKeyNote));
    if (!notes) {
        pn_set_error(error, cap, L"Not enough memory for Roblox key events.");
        return false;
    }
    size_t note_count = 0;
    uint32_t largest_id = 0;
    for (size_t index = 0; index < song->event_count; ++index)
        if (song->events[index].note_id > largest_id) largest_id = song->events[index].note_id;
    if ((size_t)largest_id + 1 > SIZE_MAX / sizeof(double)) {
        free(notes);
        pn_set_error(error, cap, L"Note duration table is too large.");
        return false;
    }
    double *ends = malloc(((size_t)largest_id + 1) * sizeof(double));
    if (!ends) {
        free(notes);
        pn_set_error(error, cap, L"Not enough memory for original note lengths.");
        return false;
    }
    for (size_t index = 0; index <= largest_id; ++index) ends[index] = -1.0;
    for (size_t index = 0; index < song->event_count; ++index) {
        PnEvent source = song->events[index];
        if (source.type == PN_NOTE_OFF) ends[source.note_id] = source.time;
    }
    for (size_t index = 0; index < song->event_count; ++index) {
        PnEvent source = song->events[index];
        if (source.type != PN_NOTE_ON) continue;
        wchar_t character;
        int slot;
        UINT vk, scan;
        bool shifted;
        if (!pn_note_character(source.data1, &character, &slot) ||
            !pn_key_info(character, &vk, &scan, &shifted)) continue;
        notes[note_count++] = (PnKeyNote){
            source.time, character, slot, vk, scan, shifted, 0,
            ends[source.note_id] > source.time ? ends[source.note_id] - source.time : 0.0
        };
    }
    free(ends);
    if (!note_count) {
        free(notes);
        pn_set_error(error, cap, L"No Roblox-compatible notes were found.");
        return false;
    }
    qsort(notes, note_count, sizeof(PnKeyNote), pn_key_note_compare);
    size_t write = 0;
    for (size_t index = 0; index < note_count; ++index) {
        if (write && fabs(notes[index].time - notes[write - 1].time) < 1e-9 &&
            notes[index].vk == notes[write - 1].vk &&
            notes[index].shifted == notes[write - 1].shifted) continue;
        notes[write++] = notes[index];
    }
    note_count = write;

    double ready[256];
    for (int index = 0; index < 256; ++index) ready[index] = -1.0;
    for (size_t index = 0; index < note_count; ++index) {
        PnKeyNote *note = &notes[index];
        double original = note->time;
        if (ready[note->vk] > note->time) note->time = ready[note->vk];
        if (note->time - original > PN_KEY_MAX_DELAY + 1e-9) {
            pn_set_error(error, cap,
                         L"Roblox key '%lc' is too dense near %.3f s. "
                         L"Use a slower speed or MIDI; no notes were discarded.",
                         note->character, original);
            free(notes);
            return false;
        }
        /* Keep the original musical release when moving an attack. */
        note->duration = fmax(0.0, note->duration - (note->time - original));
        ready[note->vk] = note->time + PN_KEY_MIN_HOLD + PN_KEY_MIN_GAP;
    }
    /* Per-key spreading may move an attack past another key's attack. */
    qsort(notes, note_count, sizeof(PnKeyNote), pn_key_note_compare);

    PnKeyEvent *events = (PnKeyEvent *)calloc(note_count * 2, sizeof(PnKeyEvent));
    if (!events) {
        free(notes);
        pn_set_error(error, cap, L"Not enough memory for the Roblox schedule.");
        return false;
    }
    double next_use[256];
    uint32_t generations[256] = {0};
    for (int index = 0; index < 256; ++index) next_use[index] = -1.0;
    for (size_t reverse = note_count; reverse-- > 0;) {
        PnKeyNote *note = &notes[reverse];
        double hold = options->hold_seconds;
        /* With expression enabled, preserve the MIDI's differentiated releases
           instead of turning every note into a fixed 20 ms keystroke. */
        if (options->humanize) hold = note->duration;
        if (hold < PN_KEY_MIN_HOLD) hold = PN_KEY_MIN_HOLD;
        if (next_use[note->vk] >= 0.0) {
            double gap = next_use[note->vk] - note->time;
            /* White/black notes may share a physical key. Release before ANY
               next use of that key so repeated and shifted notes still sound. */
            double limited = gap - PN_KEY_MIN_GAP;
            if (limited < hold) hold = limited;
        }
        /* Never undo the next-use cap by reapplying a hold floor here. */
        if (song->end_limit > 0.0 && note->time + hold > song->duration)
            hold = song->duration - note->time;
        if (hold < 0.001) {
            pn_set_error(error, cap,
                         L"Roblox key timing cannot fit before the song cutoff. "
                         L"Use a slower speed or MIDI; no notes were discarded.");
            free(events);
            free(notes);
            return false;
        }
        next_use[note->vk] = note->time;
        note->generation = ++generations[note->vk];
        events[reverse * 2] = (PnKeyEvent){
            note->time, true, note->character, note->vk, note->scan,
            note->shifted, note->generation
        };
        events[reverse * 2 + 1] = (PnKeyEvent){
            note->time + hold, false, note->character, note->vk, note->scan,
            note->shifted, note->generation
        };
    }
    free(notes);
    qsort(events, note_count * 2, sizeof(PnKeyEvent), pn_key_event_compare);
    *events_out = events;
    *count_out = note_count * 2;
    return true;
}

static bool pn_dispatch_key_event(PnClock *clock, const PnKeyEvent *event,
                                  HWND target, double duration,
                                  const PnOptions *options, double *last_progress,
                                  wchar_t *error, size_t cap) {
    for (;;) {
        if (!pn_wait_until(clock, event->time, target, duration, options,
                           last_progress, error, cap)) return false;
        if (GetForegroundWindow() != target) continue;
        if (!event->down && (!clock->held[event->vk] ||
            clock->held_generation[event->vk] != event->generation)) return true;
        if (event->down && clock->held[event->vk]) {
            pn_set_error(error, cap, L"Overlapping Roblox physical key schedule.");
            return false;
        }
        if (clock->key_known[event->vk]) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double minimum = event->down ? PN_KEY_MIN_GAP : PN_KEY_MIN_HOLD;
            LONGLONG remaining = (LONGLONG)ceil(minimum * clock->frequency.QuadPart) -
                (now.QuadPart - clock->key_changed[event->vk]);
            if (remaining > 0) {
                /* A delayed Windows wake-up must not collapse down/up/down
                   into a catch-up burst. Freeze the musical clock for this
                   required transport interval; retain every pending event. */
                LONGLONG rebased = now.QuadPart + remaining -
                    (LONGLONG)floor(event->time * clock->frequency.QuadPart);
                if (rebased > clock->started.QuadPart) clock->started.QuadPart = rebased;
                continue;
            }
        }
        if (!pn_key_transition(event, clock->held, clock->held_generation,
                               error, cap)) return false;
        LARGE_INTEGER sent;
        QueryPerformanceCounter(&sent);
        clock->key_changed[event->vk] = sent.QuadPart;
        clock->key_known[event->vk] = true;
        return true;
    }
}

bool pn_play_roblox(const PnSong *song, const PnOptions *options,
                    wchar_t *error, size_t cap) {
    wchar_t title[PN_TITLE_CAP] = L"";
    HWND target = pn_find_window(options->window_query, title);
    if (!target) {
        pn_set_error(error, cap, L"No visible window matches '%ls'.",
                     options->window_query);
        return false;
    }
    if (GetForegroundWindow() != target) {
        pn_set_error(error, cap,
                     L"The target window '%ls' is not in the foreground.", title);
        return false;
    }
    if (!options->quiet) wprintf(L"  Target window: %ls\n", title);

    PnKeyEvent *events = NULL;
    size_t event_count = 0;
    if (!pn_build_key_schedule(song, options, &events, &event_count, error, cap)) {
        return false;
    }
    bool held[256] = {false};
    uint32_t held_generation[256] = {0};
    PnClock clock;
    double last_progress = -1.0;
    bool ok = pn_clock_start(&clock, options, error, cap);
    if (!ok) {
        free(events);
        return false;
    }
    clock.held = held;
    clock.held_generation = held_generation;
    for (size_t index = 0; index < event_count && ok; ++index) {
        ok = pn_dispatch_key_event(&clock, &events[index], target, song->duration,
                                   options, &last_progress, error, cap);
    }
    pn_show_progress(ok ? song->duration : pn_clock_now(&clock),
                     song->duration, options, &last_progress, true);
    if (!options->quiet) wprintf(L"\n");
    pn_release_all_keys(held);
    pn_clock_close(&clock);
    free(events);
    return ok;
}
