#include "piano_native.h"

#include <stdarg.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

void pn_set_error(wchar_t *error, size_t cap, const wchar_t *format, ...) {
    if (!error || cap == 0) return;
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(error, cap, _TRUNCATE, format, args);
    va_end(args);
}

void pn_copy_wide(wchar_t *dest, size_t cap, const wchar_t *source) {
    if (!dest || cap == 0) return;
    if (!source) source = L"";
    wcsncpy_s(dest, cap, source, _TRUNCATE);
}

bool pn_wcs_contains_i(const wchar_t *haystack, const wchar_t *needle) {
    if (!haystack || !needle) return false;
    if (!*needle) return true;
    size_t needle_length = wcslen(needle);
    for (const wchar_t *cursor = haystack; *cursor; ++cursor) {
        if (_wcsnicmp(cursor, needle, needle_length) == 0) return true;
    }
    return false;
}

bool pn_has_extension_i(const wchar_t *path, const wchar_t *extension) {
    if (!path || !extension) return false;
    const wchar_t *dot = wcsrchr(path, L'.');
    return dot && _wcsicmp(dot, extension) == 0;
}

bool pn_utf8_to_wide(const char *bytes, size_t length,
                     wchar_t *dest, size_t cap) {
    if (!dest || cap == 0) return false;
    dest[0] = L'\0';
    if (!bytes || length == 0) return true;
    if (length > INT_MAX) return false;
    int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, bytes, (int)length, dest, (int)cap - 1
    );
    if (written <= 0) {
        written = MultiByteToWideChar(
            CP_ACP, 0, bytes, (int)length, dest, (int)cap - 1
        );
    }
    if (written <= 0) return false;
    dest[written] = L'\0';
    return true;
}

void pn_song_init(PnSong *song) {
    memset(song, 0, sizeof(*song));
    pn_copy_wide(song->title, PN_TITLE_CAP, L"Untitled");
    song->bpm = 120.0;
    song->meter_num = 4;
    song->meter_den = 4;
    song->division = 480;
}

void pn_song_free(PnSong *song) {
    if (!song) return;
    free(song->events);
    pn_song_init(song);
}

bool pn_song_add_event(PnSong *song, PnEvent event, wchar_t *error, size_t cap) {
    if (song->event_count == song->event_capacity) {
        size_t next = song->event_capacity ? song->event_capacity * 2 : 4096;
        if (next < song->event_capacity || next > SIZE_MAX / sizeof(PnEvent)) {
            pn_set_error(error, cap, L"Event array is too large.");
            return false;
        }
        PnEvent *resized = (PnEvent *)realloc(song->events, next * sizeof(PnEvent));
        if (!resized) {
            pn_set_error(error, cap, L"Not enough memory for MIDI events.");
            return false;
        }
        song->events = resized;
        song->event_capacity = next;
    }
    song->events[song->event_count++] = event;
    return true;
}

static int pn_event_rank(const PnEvent *event) {
    if (event->type == PN_CONTROL_CHANGE && event->data1 == 64) {
        return event->data2 < 64 ? 0 : 3;
    }
    if (event->type == PN_NOTE_OFF) return 1;
    if (event->type == PN_NOTE_ON) return 2;
    return 3;
}

static int pn_event_compare(const void *left_ptr, const void *right_ptr) {
    const PnEvent *left = (const PnEvent *)left_ptr;
    const PnEvent *right = (const PnEvent *)right_ptr;
    if (left->time < right->time) return -1;
    if (left->time > right->time) return 1;
    int left_rank = pn_event_rank(left);
    int right_rank = pn_event_rank(right);
    if (left_rank < right_rank) return -1;
    if (left_rank > right_rank) return 1;
    if (left->order < right->order) return -1;
    if (left->order > right->order) return 1;
    return 0;
}

void pn_song_sort(PnSong *song) {
    if (song && song->event_count > 1) {
        /* Parsers and order-preserving filters frequently leave a strictly
           sorted schedule. Avoid a full sort in that case. Exact comparator
           ties still use qsort, retaining its existing tie behavior. */
        size_t index = 1;
        while (index < song->event_count &&
               pn_event_compare(&song->events[index - 1], &song->events[index]) < 0)
            ++index;
        if (index == song->event_count) return;
        qsort(song->events, song->event_count, sizeof(PnEvent), pn_event_compare);
    }
}

static int pn_fold_note(int note) {
    while (note < PN_LOWEST_MIDI) note += 12;
    while (note > PN_HIGHEST_MIDI) note -= 12;
    return note;
}

typedef struct PnInterval {
    PnEvent on;
    PnEvent off;
    bool has_off;
} PnInterval;

static int pn_interval_compare(const void *left_ptr, const void *right_ptr) {
    const PnInterval *left = (const PnInterval *)left_ptr;
    const PnInterval *right = (const PnInterval *)right_ptr;
    if (left->on.data1 < right->on.data1) return -1;
    if (left->on.data1 > right->on.data1) return 1;
    if (left->on.time < right->on.time) return -1;
    if (left->on.time > right->on.time) return 1;
    if (left->on.order < right->on.order) return -1;
    if (left->on.order > right->on.order) return 1;
    return 0;
}

static bool pn_normalize_note_events(PnSong *song, bool coalesce,
                                     wchar_t *error, size_t cap) {
    size_t note_count = 0;
    size_t control_count = 0;
    uint32_t largest_id = 0;
    for (size_t index = 0; index < song->event_count; ++index) {
        PnEvent event = song->events[index];
        if (event.type == PN_NOTE_ON) {
            ++note_count;
            if (event.note_id > largest_id) largest_id = event.note_id;
        } else if (event.type == PN_CONTROL_CHANGE) {
            ++control_count;
        }
    }
    if (!note_count || largest_id == 0) {
        pn_set_error(error, cap, L"The song contains no paired playable notes.");
        return false;
    }
    if (note_count > SIZE_MAX / sizeof(PnInterval) ||
        (size_t)largest_id + 1 > SIZE_MAX / sizeof(size_t)) {
        pn_set_error(error, cap, L"The note-pair table is too large.");
        return false;
    }

    PnInterval *intervals = (PnInterval *)calloc(note_count, sizeof(PnInterval));
    size_t *by_id = (size_t *)malloc(((size_t)largest_id + 1) * sizeof(size_t));
    if (!intervals || !by_id) {
        free(intervals);
        free(by_id);
        pn_set_error(error, cap, L"Not enough memory to normalize note overlaps.");
        return false;
    }
    for (size_t index = 0; index <= largest_id; ++index) by_id[index] = SIZE_MAX;

    size_t interval_count = 0;
    for (size_t index = 0; index < song->event_count; ++index) {
        PnEvent event = song->events[index];
        if (event.type == PN_NOTE_ON) {
            if (!event.note_id || event.note_id > largest_id ||
                by_id[event.note_id] != SIZE_MAX) continue;
            PnInterval *interval = &intervals[interval_count];
            interval->on = event;
            interval->off = event;
            interval->off.type = PN_NOTE_OFF;
            interval->off.data2 = 0;
            interval->off.time = event.time + 0.020;
            interval->off.order = event.order + 1;
            by_id[event.note_id] = interval_count++;
        } else if (event.type == PN_NOTE_OFF && event.note_id <= largest_id) {
            size_t paired = by_id[event.note_id];
            if (paired != SIZE_MAX) {
                intervals[paired].off = event;
                intervals[paired].has_off = true;
            }
        }
    }
    free(by_id);

    qsort(intervals, interval_count, sizeof(PnInterval), pn_interval_compare);
    size_t write = 0;
    for (size_t index = 0; index < interval_count; ++index) {
        PnInterval item = intervals[index];
        if (item.off.time < item.on.time + 0.001) {
            item.off.time = item.on.time + 0.001;
        }
        if (coalesce && write && intervals[write - 1].on.data1 == item.on.data1 &&
            item.on.time - intervals[write - 1].on.time < 0.002) {
            PnInterval *previous = &intervals[write - 1];
            if (item.off.time > previous->off.time) previous->off = item.off;
            previous->off.note_id = previous->on.note_id;
            if (item.on.data2 > previous->on.data2) previous->on.data2 = item.on.data2;
            continue;
        }
        intervals[write++] = item;
    }
    interval_count = write;

    for (size_t index = 0; index + 1 < interval_count; ++index) {
        PnInterval *current = &intervals[index];
        PnInterval *following = &intervals[index + 1];
        if (current->on.data1 != following->on.data1) continue;
        if (current->off.time >= following->on.time - 0.002) {
            /* The output has one gate per pitch (also after octave folding).
               A new attack must release/re-strike that gate, but it must not
               discard the unexpired tail of another voice. Transfer ownership
               of that tail to the next attack in an overlapping chain. Never
               bridge a real rest or lengthen beyond the source overlap union. */
            if (current->off.time > following->on.time &&
                current->off.time > following->off.time) {
                following->off.time = current->off.time;
            }
            double end = following->on.time - 0.002;
            if (end < current->on.time + 0.001) end = current->on.time + 0.001;
            current->off.time = end;
        }
    }

    if (interval_count > (SIZE_MAX - control_count) / 2) {
        free(intervals);
        pn_set_error(error, cap, L"Normalized event array is too large.");
        return false;
    }
    size_t total = interval_count * 2 + control_count;
    PnEvent *rebuilt = (PnEvent *)malloc(total * sizeof(PnEvent));
    if (!rebuilt) {
        free(intervals);
        pn_set_error(error, cap, L"Not enough memory for normalized events.");
        return false;
    }
    write = 0;
    for (size_t index = 0; index < song->event_count; ++index) {
        if (song->events[index].type == PN_CONTROL_CHANGE) {
            rebuilt[write++] = song->events[index];
        }
    }
    for (size_t index = 0; index < interval_count; ++index) {
        rebuilt[write++] = intervals[index].on;
        rebuilt[write++] = intervals[index].off;
    }
    free(intervals);
    free(song->events);
    song->events = rebuilt;
    song->event_count = write;
    song->event_capacity = total;
    pn_song_sort(song);
    return true;
}

bool pn_apply_options(PnSong *song, const PnOptions *options,
                      wchar_t *error, size_t cap) {
    if (!song || !options || options->speed <= 0.0) {
        pn_set_error(error, cap, L"Speed must be greater than zero.");
        return false;
    }

    size_t write_index = 0;
    double first_note = -1.0;
    for (size_t index = 0; index < song->event_count; ++index) {
        PnEvent event = song->events[index];
        if (song->end_limit > 0.0 && event.time >= song->end_limit) {
            if (event.type == PN_NOTE_ON) continue;
            if (event.type == PN_CONTROL_CHANGE && event.time > song->end_limit + 1e-9) continue;
            if (event.type == PN_NOTE_OFF) event.time = song->end_limit;
        }
        if (event.bar > 0 && event.bar < options->from_bar) continue;
        if (event.type == PN_NOTE_ON || event.type == PN_NOTE_OFF) {
            int note = (int)event.data1 + options->transpose;
            if (note < 0 || note > 127) continue;
            if (options->fold) note = pn_fold_note(note);
            else if (note < PN_LOWEST_MIDI || note > PN_HIGHEST_MIDI) continue;
            event.data1 = (uint8_t)note;
            if (event.type == PN_NOTE_ON && first_note < 0.0) first_note = event.time;
        }
        song->events[write_index++] = event;
    }
    song->event_count = write_index;
    pn_song_sort(song);

    if (first_note < 0.0) {
        for (size_t index = 0; index < song->event_count; ++index) {
            if (song->events[index].type == PN_NOTE_ON) {
                first_note = song->events[index].time;
                break;
            }
        }
    }
    if (first_note < 0.0) {
        pn_set_error(error, cap, L"The song contains no playable notes.");
        return false;
    }

    double tempo_scale = 1.0;
    if (options->bpm_override > 0.0 && song->bpm > 0.0) {
        tempo_scale = song->bpm / options->bpm_override;
        song->bpm = options->bpm_override;
    }
    double origin = options->keep_leading_silence ? 0.0 : first_note;
    double end_limit = song->end_limit > 0.0
        ? (song->end_limit - origin) * tempo_scale / options->speed : 0.0;
    write_index = 0;
    for (size_t index = 0; index < song->event_count; ++index) {
        PnEvent event = song->events[index];
        if (event.time + 1e-9 < origin) continue;
        event.time = ((event.time - origin) * tempo_scale) / options->speed;
        if (event.time < 0.0) event.time = 0.0;
        song->events[write_index++] = event;
    }
    song->event_count = write_index;
    pn_song_sort(song);
    if (!pn_normalize_note_events(song, true, error, cap)) return false;
    if (options->humanize) {
        if (!pn_humanize_song(song, options, error, cap)) return false;
        if (!pn_normalize_note_events(song, false, error, cap)) return false;
    }
    /* Normalize the lead AFTER speed/expression so speed never stretches it. */
    double shift = 0.0;
    if (options->start_delay > 0.0) {
        for (size_t index = 0; index < song->event_count; ++index) {
            if (song->events[index].type == PN_NOTE_ON) {
                shift = options->start_delay - song->events[index].time;
                break;
            }
        }
    }
    if (end_limit > 0.0) end_limit += shift;
    for (size_t index = 0; index < song->event_count; ++index) {
        PnEvent *event = &song->events[index];
        double source_first = options->start_delay - shift;
        if (shift < 0.0 && source_first > 0.0 &&
            event->type == PN_CONTROL_CHANGE && event->time < source_first) {
            /* Compress lead-in pedal changes without collapsing their order. */
            event->time *= options->start_delay / source_first;
        } else {
            event->time = fmax(0.0, event->time + shift);
        }
        if (end_limit > 0.0 && event->time > end_limit) event->time = end_limit;
        if (end_limit > 0.0 && event->type == PN_NOTE_ON && event->time >= end_limit)
            event->time = fmax(0.0, end_limit - 0.001);
    }
    pn_song_sort(song);
    song->duration = 0.0;
    for (size_t index = 0; index < song->event_count; ++index) {
        if (song->events[index].time > song->duration) {
            song->duration = song->events[index].time;
        }
    }
    return true;
}

bool pn_load_song(const wchar_t *path, PnSong *song, wchar_t *error, size_t cap) {
    if (pn_has_extension_i(path, L".mid") || pn_has_extension_i(path, L".midi")) {
        return pn_load_midi(path, song, error, cap);
    }
    if (pn_has_extension_i(path, L".vps")) {
        return pn_load_vps(path, song, error, cap);
    }
    pn_set_error(error, cap, L"Unsupported song file: %ls", path);
    return false;
}
