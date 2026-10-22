#include "piano_native.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Internal voice/phrase decisions are tested as well as the public pipeline.
   These tests do not open a MIDI port, window, or send any keyboard input. */
#include "humanize.c"

static uint32_t next_id, next_order;
static void init(PnSong *song) {
    pn_song_init(song); song->bpm = 120.0; next_id = next_order = 0;
}
static uint32_t add_note(PnSong *song, double time, double duration, int pitch, int velocity) {
    PnEvent event = {0}; wchar_t error[PN_ERROR_CAP] = L"";
    event.time = time; event.type = PN_NOTE_ON; event.data1 = (uint8_t)pitch;
    event.data2 = (uint8_t)velocity; event.note_id = ++next_id; event.order = ++next_order;
    assert(pn_song_add_event(song, event, error, PN_ERROR_CAP));
    event.time += duration; event.type = PN_NOTE_OFF; event.data2 = 0; event.order = ++next_order;
    assert(pn_song_add_event(song, event, error, PN_ERROR_CAP));
    return next_id;
}
static void add_cc(PnSong *song, double time, int controller, int value) {
    PnEvent event = {0}; wchar_t error[PN_ERROR_CAP] = L"";
    event.time = time; event.type = PN_CONTROL_CHANGE; event.order = ++next_order;
    event.data1 = (uint8_t)controller; event.data2 = (uint8_t)value;
    assert(pn_song_add_event(song, event, error, PN_ERROR_CAP));
}
static const PnEvent *find(const PnSong *song, uint32_t id, int type) {
    for (size_t i = 0; i < song->event_count; ++i)
        if (song->events[i].type == type && song->events[i].note_id == id) return &song->events[i];
    assert(!"missing note"); return NULL;
}
static double duration(const PnSong *song, uint32_t id) {
    return find(song, id, PN_NOTE_OFF)->time - find(song, id, PN_NOTE_ON)->time;
}
static PnOptions settings(int level, bool midi) {
    PnOptions options = {0}; options.speed = 1.0; options.fold = true;
    options.from_bar = 1; options.start_delay = PN_DEFAULT_START_DELAY; options.keep_leading_silence = true;
    options.humanize = level; options.human_seed = 42; options.backend_midi = midi;
    return options;
}
static void apply(PnSong *song, int level, bool midi) {
    PnOptions options = settings(level, midi); wchar_t error[PN_ERROR_CAP] = L"";
    pn_song_sort(song); assert(pn_apply_options(song, &options, error, PN_ERROR_CAP));
}
static size_t pedals(const PnSong *song) {
    size_t count = 0;
    for (size_t i = 0; i < song->event_count; ++i)
        if (song->events[i].type == PN_CONTROL_CHANGE && song->events[i].data1 == 64) ++count;
    return count;
}

static void voice_decisions(void) {
    HumanNote notes[24] = {0}; HumanGroup groups[8] = {0}; HumanVoice voices[24] = {0};
    for (size_t g = 0; g < 8; ++g) {
        groups[g].begin = g * 3; groups[g].end = g * 3 + 3; groups[g].time = g * .5;
        for (size_t p = 0; p < 3; ++p) {
            HumanNote *n = &notes[g * 3 + p]; n->time = g * .5;
            n->pitch = (uint8_t)(p == 0 ? 48 : p == 1 ? 64 + g % 3 : 84 + g % 3);
            n->end = n->time + (p == 1 ? .49 : p == 0 ? .25 : .06);
            n->velocity = 80; n->voice = n->previous = n->next = SIZE_MAX;
        }
    }
    human_track_voices(notes, groups, 8, voices, .5);
    human_find_melody(notes, groups, 8, .5);
    for (size_t g = 0; g < 8; ++g) {
        assert(groups[g].melody == notes[g * 3 + 1].voice);
        assert(groups[g].confidence > .4); /* inner sustained melody, NOT top ornament */
    }
    puts("PASS: continuity/length-aware inner melody, higher ornaments remain background");
}

static void balance_and_articulation(void) {
    PnSong song; init(&song); uint32_t bass[8], melody[8], ornament[8];
    for (size_t g = 0; g < 8; ++g) {
        bass[g] = add_note(&song, g * .5, .25, 48, 80);
        melody[g] = add_note(&song, g * .5, .49, 64 + (int)g % 3, 80);
        ornament[g] = add_note(&song, g * .5, .06, 84 + (int)g % 3, 80);
    }
    apply(&song, 2, true);
    double contrast = 0.0;
    for (size_t g = 0; g < 8; ++g) {
        contrast += find(&song, melody[g], PN_NOTE_ON)->data2 - find(&song, ornament[g], PN_NOTE_ON)->data2;
        assert(duration(&song, ornament[g]) < .065);
        assert(duration(&song, melody[g]) > .475);
        double earliest = fmin(find(&song, bass[g], PN_NOTE_ON)->time, find(&song, melody[g], PN_NOTE_ON)->time);
        double latest = find(&song, ornament[g], PN_NOTE_ON)->time;
        assert(latest - earliest >= 0.0 && latest - earliest < .012);
    }
    assert(contrast / 8 >= 2.0); assert(pedals(&song) == 0); /* no pedal on staccato */
    printf("PASS: melody/accompaniment mean velocity contrast %.2f; staccato, legato, bounded chord spread\n", contrast / 8);
    pn_song_free(&song);
}

static void source_and_rests(void) {
    PnSong song; init(&song); song.performance_source = true;
    uint32_t soft[6], loud[6];
    for (size_t g = 0; g < 6; ++g) {
        double t = g < 3 ? g * .5 : 3.0 + (g - 3) * .5;
        soft[g] = add_note(&song, t, .18, 48, 25);
        loud[g] = add_note(&song, t, .48, 72, 110);
    }
    add_cc(&song, .02, 64, 100); add_cc(&song, .70, 64, 47);
    add_cc(&song, 3.02, 64, 85); add_cc(&song, 3.70, 64, 0);
    add_cc(&song, .50, 67, 40); /* original soft pedal preserved */
    apply(&song, 2, true); assert(pedals(&song) == 4);
    for (size_t g = 0; g < 6; ++g) {
        assert(abs(find(&song, soft[g], PN_NOTE_ON)->data2 - 25) <= 2);
        assert(abs(find(&song, loud[g], PN_NOTE_ON)->data2 - 110) <= 2);
    }
    assert(find(&song, loud[3], PN_NOTE_ON)->time - find(&song, loud[2], PN_NOTE_OFF)->time > 1.5);
    assert(fabs(find(&song, loud[3], PN_NOTE_ON)->time - (PN_DEFAULT_START_DELAY + 3.0)) < .006);
    int values[] = {100,47,85,0}; size_t p = 0;
    for (size_t i = 0; i < song.event_count; ++i) {
        const PnEvent *e = &song.events[i];
        if (e->type == PN_CONTROL_CHANGE && e->data1 == 64) assert(e->data2 == values[p++]);
    }
    assert(p == 4); pn_song_free(&song);
    puts("PASS: source pp/ff and half-pedal preserved; real rest and phrase entry retained");
}

static void pedal_cycles(void) {
    for (int variant = 0; variant < 4; ++variant) {
        PnSong song; init(&song);
        song.performance_source = variant == 1;
        for (size_t g = 0; g < 8; ++g) {
            add_note(&song, g * .5, .43, 48, 65);
            add_note(&song, g * .5, .43, 55, 65);
            add_note(&song, g * .5, .49, 72 + (int)g % 3, 80);
        }
        if (variant == 3) { add_cc(&song, .03, 64, 96); add_cc(&song, 3.97, 64, 0); }
        apply(&song, 2, variant != 2);
        if (variant == 0) {
            assert(pedals(&song) >= 4);
            bool down = false; double start = 0.0;
            for (size_t i = 0; i < song.event_count; ++i) {
                const PnEvent *e = &song.events[i];
                if (e->type != PN_CONTROL_CHANGE || e->data1 != 64) continue;
                if (e->data2 >= 64) { assert(!down); start = e->time; down = true; }
                else { assert(down && e->time - start <= 1.2); down = false; }
            }
            assert(!down);
        } else assert(pedals(&song) == (variant == 3 ? 2 : 0));
        pn_song_free(&song);
    }
    puts("PASS: conservative after-attack MIDI pedal cycles, no invented performance/Roblox pedal");
}

static void fast_repeats_and_cutoff(void) {
    PnSong song; init(&song); song.end_limit = 3.0;
    uint32_t ids[120];
    for (size_t i = 0; i < 120; ++i) ids[i] = add_note(&song, i * .025, .022, 60, 80);
    add_cc(&song, 3.0, 64, 0);
    apply(&song, 3, true);
    for (size_t i = 0; i < 120; ++i) {
        const PnEvent *on = find(&song, ids[i], PN_NOTE_ON);
        const PnEvent *off = find(&song, ids[i], PN_NOTE_OFF);
        assert(fabs(on->time - (PN_DEFAULT_START_DELAY + i * .025)) < .004);
        assert(off->time > on->time && duration(&song, ids[i]) < .025);
        if (i) assert(on->time >= find(&song, ids[i - 1], PN_NOTE_OFF)->time);
    }
    assert(fabs(song.duration - (PN_DEFAULT_START_DELAY + 3.0)) < 1e-9); pn_song_free(&song);
    puts("PASS: 40 notes/second repeats, no overlap, fixed first note and cutoff");
}

static void breathing_and_original(void) {
    for (int level = 0; level <= 2; level += 2) {
        PnSong song; init(&song); uint32_t ids[12];
        for (size_t i = 0; i < 10; ++i) ids[i] = add_note(&song, i * .5, .49, 60 + (int)i % 4, 80);
        ids[10] = add_note(&song, 6.0, .49, 60, 80);
        ids[11] = add_note(&song, 6.5, .49, 62, 80);
        apply(&song, level, true);
        if (level == 0) {
            for (size_t i = 0; i < 10; ++i) {
                assert(fabs(find(&song, ids[i], PN_NOTE_ON)->time - (PN_DEFAULT_START_DELAY + i * .5)) < 1e-9);
                assert(fabs(duration(&song, ids[i]) - .49) < 1e-9);
                assert(find(&song, ids[i], PN_NOTE_ON)->data2 == 80);
            }
        } else {
            assert(find(&song, ids[2], PN_NOTE_ON)->time < PN_DEFAULT_START_DELAY + 1.0);
            assert(find(&song, ids[9], PN_NOTE_ON)->time > PN_DEFAULT_START_DELAY + 4.52);
            assert(fabs(find(&song, ids[10], PN_NOTE_ON)->time - (PN_DEFAULT_START_DELAY + 6.0)) < 1e-9);
            assert(find(&song, ids[10], PN_NOTE_ON)->time - find(&song, ids[9], PN_NOTE_OFF)->time > .95);
        }
        pn_song_free(&song);
    }
    puts("PASS: early phrase push, relaxed cadence, pinned next entry, original mode unchanged");
}

int main(void) {
    voice_decisions(); balance_and_articulation(); source_and_rests();
    pedal_cycles(); fast_repeats_and_cutoff();
    breathing_and_original();
    puts("ALL STRUCTURAL EXPRESSION TESTS PASSED"); return 0;
}
