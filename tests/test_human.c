#include "piano_native.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static size_t changed;
static double max_delta;
static bool midi_backend;
static size_t added_pedals;
static void check(bool ok, const wchar_t *path, const char *reason) {
    if (ok) return;
    if (failures++ < 10) fwprintf(stderr, L"FAIL %ls: %hs\n", path, reason);
}

static bool prepare(const wchar_t *path, int level, uint32_t seed, double speed,
                    PnSong *song) {
    wchar_t error[PN_ERROR_CAP] = L"";
    PnOptions options = {0};
    options.speed = speed; options.fold = true; options.from_bar = 1;
    options.start_delay = PN_DEFAULT_START_DELAY;
    options.keep_leading_silence = true;
    options.humanize = level; options.human_seed = seed;
    options.backend_midi = midi_backend;
    if (!pn_load_song(path, song, error, PN_ERROR_CAP)) return false;
    if (pn_apply_options(song, &options, error, PN_ERROR_CAP)) return true;
    fwprintf(stderr, L"%ls\n", error);
    pn_song_free(song); return false;
}

static void validate(const PnSong *original, const PnSong *human, const wchar_t *path) {
    for (size_t i = 0; i < human->event_count; ++i) {
        if (human->events[i].type != PN_NOTE_ON) continue;
        check(fabs(human->events[i].time - PN_DEFAULT_START_DELAY) < 1e-9, path, "first note is not at 1.2 seconds");
        break;
    }
    size_t original_notes = 0, human_notes = 0, original_pedals = 0;
    for (size_t i = 0; i < original->event_count; ++i) {
        if (original->events[i].type != PN_CONTROL_CHANGE) ++original_notes;
        else if (original->events[i].data1 == 64) ++original_pedals;
    }
    for (size_t i = 0; i < human->event_count; ++i)
        if (human->events[i].type != PN_CONTROL_CHANGE) ++human_notes;
    check(original_notes == human_notes, path, "lost or added notes");
    bool can_add_pedal = midi_backend && !original->performance_source && !original_pedals;
    if (!can_add_pedal) check(original->event_count == human->event_count, path, "lost or added events");
    else check(human->event_count >= original->event_count, path, "lost controls");
    uint32_t max_id = 0;
    for (size_t i = 0; i < original->event_count; ++i)
        if (original->events[i].note_id > max_id) max_id = original->events[i].note_id;
    const PnEvent **ons = calloc((size_t)max_id + 1, sizeof(*ons));
    const PnEvent **offs = calloc((size_t)max_id + 1, sizeof(*offs));
    for (size_t i = 0; i < human->event_count; ++i) {
        const PnEvent *e = &human->events[i];
        check(isfinite(e->time) && e->time >= 0, path, "invalid time");
        if (i) check(e->time >= human->events[i - 1].time, path, "unsorted events");
        if (e->note_id > max_id) { check(false, path, "changed note identity"); continue; }
        if (e->type == PN_NOTE_ON) {
            check(e->data2 >= 1 && e->data2 <= 127, path, "invalid velocity");
            ons[e->note_id] = e;
        } else if (e->type == PN_NOTE_OFF) {
            offs[e->note_id] = e;
        }
    }
    double active_until[128] = {0};
    for (size_t i = 0; i < human->event_count; ++i) {
        const PnEvent *e = &human->events[i];
        if (e->type != PN_NOTE_ON || e->note_id > max_id) continue;
        check(e->time >= active_until[e->data1] - 1e-9, path, "same-note overlap");
        if (offs[e->note_id]) active_until[e->data1] = offs[e->note_id]->time;
    }
    for (size_t i = 0; i < original->event_count; ++i) {
        const PnEvent *old = &original->events[i];
        if (old->type != PN_NOTE_ON) continue;
        const PnEvent *on = ons[old->note_id], *off = offs[old->note_id];
        check(on && off, path, "missing note pair");
        if (!on || !off) continue;
        check(on->data1 == old->data1 && on->channel == old->channel, path, "changed pitch/channel");
        check(off->time > on->time, path, "nonpositive duration");
        double delta = fabs(on->time - old->time);
        if (delta > max_delta) max_delta = delta;
        check(delta < 0.075, path, "timing perturbation too large");
        if (delta > 1e-8) ++changed;
    }
    /* Match source CC identity (order is preserved) even when inferred CC64
       cycles were inserted between source controls. */
    uint32_t max_order = 0;
    for (size_t i = 0; i < original->event_count; ++i) {
        if (original->events[i].order > max_order) max_order = original->events[i].order;
    }
    const PnEvent **controls = calloc((size_t)max_order + 1, sizeof(*controls));
    for (size_t i = 0; i < human->event_count; ++i) {
        const PnEvent *e = &human->events[i];
        if (e->type == PN_CONTROL_CHANGE && e->order <= max_order) controls[e->order] = e;
    }
    for (size_t i = 0; i < original->event_count; ++i) {
        const PnEvent *old = &original->events[i];
        if (old->type != PN_CONTROL_CHANGE) continue;
        const PnEvent *match = controls[old->order];
        check(match != NULL && match->data1 == old->data1 && match->data2 == old->data2 &&
              match->channel == old->channel, path, "lost/changed source pedal/control");
        if (match) check(fabs(match->time - old->time) < .075, path, "source control moved too far");
    }
    if (can_add_pedal) {
        bool down = false; double down_time = 0.0; size_t new_controls = 0;
        for (size_t i = 0; i < human->event_count; ++i) {
            const PnEvent *e = &human->events[i];
            if (e->type != PN_CONTROL_CHANGE || e->data1 != 64) continue;
            if (e->data2 >= 64) {
                check(!down, path, "duplicate synthesized pedal down"); down = true; down_time = e->time;
                size_t sounding = 0;
                for (uint32_t id = 1; id <= max_id; ++id)
                    if (ons[id] && offs[id] && ons[id]->time < down_time && offs[id]->time > down_time) ++sounding;
                check(sounding >= 3, path, "synthesized pedal not following a held chord");
            } else {
                check(down && e->time > down_time && e->time - down_time <= 1.2 + 1e-9,
                      path, "unpaired/too-long synthesized pedal"); down = false;
            }
            ++added_pedals;
            ++new_controls;
        }
        check(human->event_count - original->event_count == new_controls, path, "unexpected extra controller");
        check(!down, path, "synthesized pedal left down");
        down = false;
        for (size_t i = 0; i < human->event_count; ++i) {
            const PnEvent *e = &human->events[i];
            if (e->type == PN_CONTROL_CHANGE && e->data1 == 64) down = e->data2 >= 64;
            if (e->type == PN_NOTE_ON && down) {
                double lift = -1.0;
                for (size_t j = i + 1; j < human->event_count; ++j) {
                    const PnEvent *next = &human->events[j];
                    if (next->type == PN_CONTROL_CHANGE && next->data1 == 64 && next->data2 < 64) {
                        lift = next->time; break;
                    }
                }
                check(lift >= e->time && lift - e->time <= .008 + 1e-9,
                      path, "inferred repedal overlap exceeded 8ms");
            }
        }
    }
    check(fabs(original->duration - human->duration) < 0.075, path, "accumulated tempo drift");
    free(ons); free(offs); free(controls);
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 2 || argc > 3) return 2;
    midi_backend = argc == 3 && wcscmp(argv[2], L"--midi") == 0;
    PnSongList list = {0}; wchar_t error[PN_ERROR_CAP] = L"";
    if (!pn_discover_songs(argv[1], &list, error, PN_ERROR_CAP)) return 2;
    size_t cases = 0;
    for (size_t i = 0; i < list.count; ++i) {
        const wchar_t *path = list.items[i].path;
        static const double speeds[] = {.75, .8, .9, 1.0, 1.1, 1.25};
        for (size_t speed_case = 0; speed_case < sizeof(speeds) / sizeof(speeds[0]); ++speed_case) {
            double speed = speeds[speed_case];
            PnSong original;
            if (!prepare(path, 0, 42, speed, &original)) { check(false, path, "parse failure"); continue; }
            for (int level = 1; level <= 3; ++level) {
                PnSong human, repeat;
                if (!prepare(path, level, 42, speed, &human)) { check(false, path, "humanize failure"); continue; }
                validate(&original, &human, path);
                if (prepare(path, level, 42, speed, &repeat)) {
                    check(human.event_count == repeat.event_count &&
                          memcmp(human.events, repeat.events, human.event_count * sizeof(PnEvent)) == 0,
                          path, "seed not repeatable");
                    pn_song_free(&repeat);
                } else check(false, path, "repeat parse failure");
                pn_song_free(&human); ++cases;
            }
            pn_song_free(&original);
        }
    }
    printf("%zu songs; %zu cases; backend %s; %zu changed onsets; max delta %.3f ms; %zu added pedal events; %d failures\n",
           list.count, cases, midi_backend ? "MIDI" : "Roblox", changed, max_delta * 1000, added_pedals, failures);
    pn_song_list_free(&list);
    return failures ? 1 : 0;
}
