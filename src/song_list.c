#include "piano_native.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static bool pn_list_reserve(PnSongList *list, size_t needed,
                            wchar_t *error, size_t cap) {
    if (needed <= list->capacity) return true;
    size_t next = list->capacity ? list->capacity * 2 : 128;
    while (next < needed) next *= 2;
    PnSongPath *resized = (PnSongPath *)realloc(
        list->items, next * sizeof(PnSongPath)
    );
    if (!resized) {
        pn_set_error(error, cap, L"Not enough memory for the song list.");
        return false;
    }
    list->items = resized;
    list->capacity = next;
    return true;
}

static void pn_filename_stem(const wchar_t *filename, wchar_t *stem, size_t cap) {
    pn_copy_wide(stem, cap, filename);
    wchar_t *dot = wcsrchr(stem, L'.');
    if (dot) *dot = L'\0';
}

static void pn_fallback_title(const wchar_t *filename, wchar_t *title, size_t cap) {
    pn_filename_stem(filename, title, cap);
}

static bool pn_add_discovered(const wchar_t *songs_dir,
                              const WIN32_FIND_DATAW *data, int priority,
                              PnSongList *list, wchar_t *error, size_t cap) {
    if (data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return true;
    wchar_t stem[MAX_PATH];
    pn_filename_stem(data->cFileName, stem, MAX_PATH);
    size_t target = list->count;
    for (size_t index = 0; index < list->count; ++index) {
        if (_wcsicmp(list->items[index].stem, stem) == 0) {
            if (list->items[index].priority >= priority) return true;
            target = index;
            break;
        }
    }
    if (target == list->count && !pn_list_reserve(list, list->count + 1, error, cap)) {
        return false;
    }
    PnSongPath *entry = &list->items[target];
    memset(entry, 0, sizeof(*entry));
    if (_snwprintf_s(entry->path, PN_PATH_CAP, _TRUNCATE,
                     L"%ls\\%ls", songs_dir, data->cFileName) < 0) {
        pn_set_error(error, cap, L"Song path is too long: %ls", data->cFileName);
        return false;
    }
    pn_copy_wide(entry->stem, MAX_PATH, stem);
    pn_copy_wide(entry->category, PN_TITLE_CAP, PN_TEXT(L"기타", L"Other"));
    entry->category_order = 999;
    entry->priority = priority;
    PnSong song;
    wchar_t load_error[PN_ERROR_CAP] = L"";
    if (pn_load_song(entry->path, &song, load_error, PN_ERROR_CAP)) {
        pn_copy_wide(entry->title, PN_TITLE_CAP, song.title);
        pn_song_free(&song);
    } else {
        pn_fallback_title(data->cFileName, entry->title, PN_TITLE_CAP);
    }
    if (target == list->count) ++list->count;
    return true;
}

static void pn_strip_line_end(char *line) {
    size_t length = strlen(line);
    while (length && (line[length - 1] == '\r' || line[length - 1] == '\n')) {
        line[--length] = '\0';
    }
}

static void pn_load_categories(const wchar_t *songs_dir, PnSongList *list) {
    wchar_t path[PN_PATH_CAP];
    if (_snwprintf_s(path, PN_PATH_CAP, _TRUNCATE,
                     L"%ls\\song_categories.tsv", songs_dir) < 0) return;
    FILE *file = _wfopen(path, L"rb");
    if (!file) return;

    char line[4096];
    while (fgets(line, (int)sizeof(line), file)) {
        pn_strip_line_end(line);
        char *cursor = line;
        if ((unsigned char)cursor[0] == 0xEF &&
            (unsigned char)cursor[1] == 0xBB &&
            (unsigned char)cursor[2] == 0xBF) cursor += 3;
        if (!*cursor || *cursor == '#') continue;

        char *first_tab = strchr(cursor, '\t');
        if (!first_tab) continue;
        *first_tab = '\0';
        char *category_utf8 = first_tab + 1;
        char *second_tab = strchr(category_utf8, '\t');
        if (!second_tab) continue;
        *second_tab = '\0';
        char *stem_utf8 = second_tab + 1;
        if (!*category_utf8 || !*stem_utf8) continue;

        char *end = NULL;
        long parsed_order = strtol(cursor, &end, 10);
        if (end == cursor || *end || parsed_order < 0 || parsed_order > INT_MAX) continue;
        wchar_t category[PN_TITLE_CAP];
        wchar_t stem[MAX_PATH];
        if (!pn_utf8_to_wide(category_utf8, strlen(category_utf8),
                             category, PN_TITLE_CAP) ||
            !pn_utf8_to_wide(stem_utf8, strlen(stem_utf8), stem, MAX_PATH)) continue;

        for (size_t index = 0; index < list->count; ++index) {
            if (_wcsicmp(list->items[index].stem, stem) != 0) continue;
            list->items[index].category_order = (int)parsed_order;
            pn_copy_wide(list->items[index].category, PN_TITLE_CAP, category);
            break;
        }
    }
    fclose(file);
}

static void pn_load_titles(const wchar_t *songs_dir, PnSongList *list) {
    wchar_t path[PN_PATH_CAP];
    if (_snwprintf_s(path, PN_PATH_CAP, _TRUNCATE,
                     L"%ls\\song_titles_ko.tsv", songs_dir) < 0) return;
    FILE *file = _wfopen(path, L"rb");
    if (!file) return;

    char line[4096];
    while (fgets(line, (int)sizeof(line), file)) {
        pn_strip_line_end(line);
        char *cursor = line;
        if ((unsigned char)cursor[0] == 0xEF &&
            (unsigned char)cursor[1] == 0xBB &&
            (unsigned char)cursor[2] == 0xBF) cursor += 3;
        if (!*cursor || *cursor == '#') continue;

        char *tab = strchr(cursor, '\t');
        if (!tab) continue;
        *tab = '\0';
        char *title_utf8 = tab + 1;
        if (!*cursor || !*title_utf8) continue;

        wchar_t stem[MAX_PATH];
        wchar_t title[PN_TITLE_CAP];
        if (!pn_utf8_to_wide(cursor, strlen(cursor), stem, MAX_PATH) ||
            !pn_utf8_to_wide(title_utf8, strlen(title_utf8),
                             title, PN_TITLE_CAP)) continue;
        for (size_t index = 0; index < list->count; ++index) {
            if (_wcsicmp(list->items[index].stem, stem) != 0) continue;
            pn_copy_wide(list->items[index].title, PN_TITLE_CAP, title);
            break;
        }
    }
    fclose(file);
}

static void pn_load_sort_keys(const wchar_t *songs_dir, PnSongList *list) {
    for (size_t index = 0; index < list->count; ++index) {
        pn_copy_wide(list->items[index].sort_key, PN_TITLE_CAP,
                     list->items[index].title);
    }

    wchar_t path[PN_PATH_CAP];
    if (_snwprintf_s(path, PN_PATH_CAP, _TRUNCATE,
                     L"%ls\\song_sort_keys.tsv", songs_dir) < 0) return;
    FILE *file = _wfopen(path, L"rb");
    if (!file) return;

    char line[4096];
    while (fgets(line, (int)sizeof(line), file)) {
        pn_strip_line_end(line);
        char *cursor = line;
        if ((unsigned char)cursor[0] == 0xEF &&
            (unsigned char)cursor[1] == 0xBB &&
            (unsigned char)cursor[2] == 0xBF) cursor += 3;
        if (!*cursor || *cursor == '#') continue;

        char *tab = strchr(cursor, '\t');
        if (!tab) continue;
        *tab = '\0';
        char *sort_key_utf8 = tab + 1;
        if (!*cursor || !*sort_key_utf8) continue;

        wchar_t stem[MAX_PATH];
        wchar_t sort_key[PN_TITLE_CAP];
        if (!pn_utf8_to_wide(cursor, strlen(cursor), stem, MAX_PATH) ||
            !pn_utf8_to_wide(sort_key_utf8, strlen(sort_key_utf8),
                             sort_key, PN_TITLE_CAP)) continue;
        for (size_t index = 0; index < list->count; ++index) {
            if (_wcsicmp(list->items[index].stem, stem) != 0) continue;
            pn_copy_wide(list->items[index].sort_key, PN_TITLE_CAP, sort_key);
            break;
        }
    }
    fclose(file);
}

static void pn_load_artists(const wchar_t *songs_dir, PnSongList *list) {
    for (size_t index = 0; index < list->count; ++index) {
        PnSongPath *song = &list->items[index];
        const wchar_t *separator = wcsstr(song->sort_key, L" - ");
        pn_copy_wide(song->artist, PN_TITLE_CAP, PN_UNKNOWN_ARTIST);
        if (!separator || separator == song->sort_key) continue;
        size_t length = (size_t)(separator - song->sort_key);
        if (length >= PN_TITLE_CAP) continue;
        memcpy(song->artist, song->sort_key, length * sizeof(wchar_t));
        song->artist[length] = L'\0';
        /* These title prefixes identify a work/franchise, not its author. */
        const wchar_t *non_artists[] = {L"Genshin Impact", L"Limbus Company", L"Oldboy",
            L"Secret Piano Battle", L"Sonic Mania", L"He's a Pirate", L"Take Five"};
        for (size_t n = 0; n < sizeof(non_artists) / sizeof(non_artists[0]); ++n)
            if (_wcsicmp(song->artist, non_artists[n]) == 0)
                pn_copy_wide(song->artist, PN_TITLE_CAP, PN_UNKNOWN_ARTIST);
        if (_wcsicmp(song->artist, L"Dmitri Shostakovich") == 0)
            pn_copy_wide(song->artist, PN_TITLE_CAP, L"Shostakovich");
        if (_wcsicmp(song->artist, L"Erik Satie") == 0)
            pn_copy_wide(song->artist, PN_TITLE_CAP, L"Satie");
        if (_wcsicmp(song->artist, L"Astor Piazzolla") == 0)
            pn_copy_wide(song->artist, PN_TITLE_CAP, L"Piazzolla");
    }
    wchar_t path[PN_PATH_CAP];
    if (_snwprintf_s(path, PN_PATH_CAP, _TRUNCATE, L"%ls\\song_artists.tsv", songs_dir) < 0) return;
    FILE *file = _wfopen(path, L"rb");
    if (!file) return;
    char line[4096];
    while (fgets(line, sizeof(line), file)) {
        pn_strip_line_end(line);
        char *cursor = line;
        if ((unsigned char)cursor[0] == 0xEF && (unsigned char)cursor[1] == 0xBB &&
            (unsigned char)cursor[2] == 0xBF) cursor += 3;
        if (!*cursor || *cursor == '#') continue;
        char *tab = strchr(cursor, '\t');
        if (!tab) continue;
        *tab = '\0';
        wchar_t stem[MAX_PATH], artist[PN_TITLE_CAP];
        if (!*(tab + 1) || !pn_utf8_to_wide(cursor, strlen(cursor), stem, MAX_PATH) ||
            !pn_utf8_to_wide(tab + 1, strlen(tab + 1), artist, PN_TITLE_CAP)) continue;
        for (size_t index = 0; index < list->count; ++index)
            if (_wcsicmp(list->items[index].stem, stem) == 0) {
                pn_copy_wide(list->items[index].artist, PN_TITLE_CAP, artist);
                break;
            }
    }
    fclose(file);
}

static void pn_load_aliases(const wchar_t *songs_dir, PnSongList *list) {
    wchar_t path[PN_PATH_CAP];
    if (_snwprintf_s(path, PN_PATH_CAP, _TRUNCATE,
                     L"%ls\\song_search_aliases.tsv", songs_dir) < 0) return;
    FILE *file = _wfopen(path, L"rb");
    if (!file) return;

    char line[8192];
    while (fgets(line, (int)sizeof(line), file)) {
        pn_strip_line_end(line);
        char *cursor = line;
        if ((unsigned char)cursor[0] == 0xEF &&
            (unsigned char)cursor[1] == 0xBB &&
            (unsigned char)cursor[2] == 0xBF) cursor += 3;
        if (!*cursor || *cursor == '#') continue;

        char *tab = strchr(cursor, '\t');
        if (!tab) continue;
        *tab = '\0';
        char *aliases_utf8 = tab + 1;
        if (!*cursor || !*aliases_utf8) continue;

        wchar_t stem[MAX_PATH];
        wchar_t aliases[PN_ALIAS_CAP];
        if (!pn_utf8_to_wide(cursor, strlen(cursor), stem, MAX_PATH) ||
            !pn_utf8_to_wide(aliases_utf8, strlen(aliases_utf8),
                             aliases, PN_ALIAS_CAP)) continue;
        for (size_t index = 0; index < list->count; ++index) {
            if (_wcsicmp(list->items[index].stem, stem) != 0) continue;
            pn_copy_wide(list->items[index].aliases, PN_ALIAS_CAP, aliases);
            break;
        }
    }
    fclose(file);
}

static bool pn_scan_pattern(const wchar_t *songs_dir, const wchar_t *pattern,
                            int priority, PnSongList *list,
                            wchar_t *error, size_t cap) {
    wchar_t search[PN_PATH_CAP];
    if (_snwprintf_s(search, PN_PATH_CAP, _TRUNCATE,
                     L"%ls\\%ls", songs_dir, pattern) < 0) {
        pn_set_error(error, cap, L"Songs directory path is too long.");
        return false;
    }
    WIN32_FIND_DATAW data;
    HANDLE handle = FindFirstFileW(search, &data);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        return code == ERROR_FILE_NOT_FOUND;
    }
    bool ok = true;
    do {
        if (!pn_add_discovered(songs_dir, &data, priority, list, error, cap)) {
            ok = false;
            break;
        }
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
    return ok;
}

static int pn_song_path_compare(const void *left_ptr, const void *right_ptr) {
    const PnSongPath *left = (const PnSongPath *)left_ptr;
    const PnSongPath *right = (const PnSongPath *)right_ptr;
    if (left->category_order < right->category_order) return -1;
    if (left->category_order > right->category_order) return 1;
    int category_result = CompareStringOrdinal(left->category, -1, right->category, -1, TRUE);
    if (category_result == CSTR_LESS_THAN) return -1;
    if (category_result == CSTR_GREATER_THAN) return 1;
    bool left_unknown = _wcsicmp(left->artist, L"미분류") == 0 ||
                        _wcsicmp(left->artist, L"Unknown artist") == 0;
    bool right_unknown = _wcsicmp(right->artist, L"미분류") == 0 ||
                         _wcsicmp(right->artist, L"Unknown artist") == 0;
    if (left_unknown != right_unknown) return left_unknown ? 1 : -1;
    int result = CompareStringOrdinal(left->artist, -1, right->artist, -1, TRUE);
    if (result == CSTR_LESS_THAN) return -1;
    if (result == CSTR_GREATER_THAN) return 1;
    result = CompareStringOrdinal(left->title, -1, right->title, -1, TRUE);
    if (result == CSTR_LESS_THAN) return -1;
    if (result == CSTR_GREATER_THAN) return 1;
    return _wcsicmp(left->path, right->path);
}

bool pn_discover_songs(const wchar_t *songs_dir, PnSongList *list,
                       wchar_t *error, size_t cap) {
    memset(list, 0, sizeof(*list));
    if (!pn_scan_pattern(songs_dir, L"*.vps", 1, list, error, cap) ||
        !pn_scan_pattern(songs_dir, L"*.midi", 2, list, error, cap) ||
        !pn_scan_pattern(songs_dir, L"*.mid", 3, list, error, cap)) {
        pn_song_list_free(list);
        return false;
    }
    if (list->count == 0) {
        /* A new public installation intentionally has no music bundled. */
        return true;
    }
    pn_load_categories(songs_dir, list);
    pn_load_titles(songs_dir, list);
    pn_load_sort_keys(songs_dir, list);
    pn_load_artists(songs_dir, list);
    pn_load_aliases(songs_dir, list);
    qsort(list->items, list->count, sizeof(PnSongPath), pn_song_path_compare);
    return true;
}

void pn_song_list_free(PnSongList *list) {
    if (!list) return;
    free(list->items);
    memset(list, 0, sizeof(*list));
}
