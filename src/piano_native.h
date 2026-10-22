#ifndef PIANO_NATIVE_H
#define PIANO_NATIVE_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <mmsystem.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define PN_TITLE_CAP 512
#define PN_ALIAS_CAP 2048
#define PN_ERROR_CAP 1024
#define PN_PATH_CAP 32768
#define PN_LOWEST_MIDI 36
#define PN_HIGHEST_MIDI 96
#define PN_DEFAULT_START_DELAY 1.2

/* Both editions share the same engine. Only interface text changes. */
#ifdef PN_LANG_EN
#define PN_TEXT(ko, en) en
#else
#define PN_TEXT(ko, en) ko
#endif
#define PN_UNKNOWN_ARTIST PN_TEXT(L"미분류", L"Unknown artist")

typedef enum PnEventType {
    PN_NOTE_OFF = 0,
    PN_CONTROL_CHANGE = 1,
    PN_NOTE_ON = 2
} PnEventType;

typedef struct PnEvent {
    double time;
    uint64_t tick;
    uint32_t order;
    int bar;
    uint8_t type;
    uint8_t channel;
    uint8_t data1;
    uint8_t data2;
    uint32_t note_id;
} PnEvent;

typedef struct PnSong {
    wchar_t title[PN_TITLE_CAP];
    PnEvent *events;
    size_t event_count;
    size_t event_capacity;
    double duration;
    double bpm;
    int meter_num;
    int meter_den;
    int division;
    bool performance_source;
    double end_limit;
} PnSong;

typedef void (*PnProgressCallback)(void *user, double elapsed, double total);
typedef void (*PnPauseCallback)(void *user, bool paused);

typedef struct PnOptions {
    double speed;
    double hold_seconds;
    double start_delay;
    LONGLONG start_counter;
    int humanize; /* 0 original, 1 subtle, 2 natural, 3 expressive */
    uint32_t human_seed;
    double bpm_override;
    int transpose;
    int from_bar;
    bool fold;
    bool keep_leading_silence;
    bool quiet;
    bool dry_run;
    bool validate_all;
    bool list_only;
    bool backend_midi;
    wchar_t port_query[PN_TITLE_CAP];
    wchar_t window_query[PN_TITLE_CAP];
    wchar_t songs_dir[PN_PATH_CAP];
    wchar_t direct_song[PN_PATH_CAP];
    HANDLE stop_event;
    PnProgressCallback progress_callback;
    PnPauseCallback pause_callback;
    void *progress_user;
} PnOptions;

typedef struct PnSongPath {
    wchar_t path[PN_PATH_CAP];
    wchar_t stem[MAX_PATH];
    wchar_t title[PN_TITLE_CAP];
    wchar_t sort_key[PN_TITLE_CAP];
    wchar_t artist[PN_TITLE_CAP];
    wchar_t category[PN_TITLE_CAP];
    wchar_t aliases[PN_ALIAS_CAP];
    int category_order;
    int priority;
} PnSongPath;

typedef struct PnSongList {
    PnSongPath *items;
    size_t count;
    size_t capacity;
} PnSongList;

void pn_song_init(PnSong *song);
void pn_song_free(PnSong *song);
bool pn_song_add_event(PnSong *song, PnEvent event, wchar_t *error, size_t cap);
void pn_song_sort(PnSong *song);
double pn_human_strength(const PnSong *song, const PnOptions *options);
double pn_human_noise(uint32_t seed, uint32_t id);
bool pn_humanize_song(PnSong *song, const PnOptions *options,
                      wchar_t *error, size_t cap);
bool pn_apply_options(PnSong *song, const PnOptions *options,
                      wchar_t *error, size_t cap);

bool pn_load_midi(const wchar_t *path, PnSong *song, wchar_t *error, size_t cap);
bool pn_load_vps(const wchar_t *path, PnSong *song, wchar_t *error, size_t cap);
bool pn_load_song(const wchar_t *path, PnSong *song, wchar_t *error, size_t cap);

bool pn_discover_songs(const wchar_t *songs_dir, PnSongList *list,
                       wchar_t *error, size_t cap);
void pn_song_list_free(PnSongList *list);

bool pn_play_midi(const PnSong *song, const PnOptions *options,
                  wchar_t *error, size_t cap);
bool pn_play_roblox(const PnSong *song, const PnOptions *options,
                    wchar_t *error, size_t cap);
bool pn_focus_window(const wchar_t *fragment, wchar_t *error, size_t cap);
void pn_midi_panic(HMIDIOUT handle);

bool pn_wcs_contains_i(const wchar_t *haystack, const wchar_t *needle);
bool pn_has_extension_i(const wchar_t *path, const wchar_t *extension);
void pn_copy_wide(wchar_t *dest, size_t cap, const wchar_t *source);
bool pn_utf8_to_wide(const char *bytes, size_t length,
                     wchar_t *dest, size_t cap);
void pn_set_error(wchar_t *error, size_t cap, const wchar_t *format, ...);

#endif
