#include "piano_native.h"

#include <math.h>
#include <stdlib.h>

#define HUMAN_ACTIVE_VOICES 64
#define HUMAN_PI 3.141592653589793

typedef struct HumanNote {
    size_t on, off, group, voice, previous, next;
    double time, end, attack_offset, release_floor;
    double voice_pitch, voice_duration, voice_velocity;
    size_t voice_notes;
    uint32_t id;
    uint8_t pitch, channel, velocity;
} HumanNote;

typedef struct HumanVoice {
    size_t last;
} HumanVoice;

typedef struct HumanGroup {
    size_t begin, end, phrase, melody;
    double time, mapped, confidence, balance, shape;
    bool sustained;
} HumanGroup;

static double human_clamp(double value, double low, double high) {
    return fmax(low, fmin(high, value));
}

/* No mutable RNG: the same seed produces an identical offline schedule. */
double pn_human_noise(uint32_t seed, uint32_t id) {
    uint32_t x = seed ^ (id * 0x9e3779b9u) ^ 0x85ebca6bu;
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return (double)x / 4294967295.0 * 2.0 - 1.0;
}

double pn_human_strength(const PnSong *song, const PnOptions *options) {
    static const double strengths[] = {0.0, 0.45, 1.0, 1.6};
    if (options->humanize < 0 || options->humanize > 3) return 0.0;
    return strengths[options->humanize] * (song->performance_source ? 0.55 : 1.0);
}

static double human_voice_cost(const HumanNote *note, const HumanNote *previous,
                               double horizon) {
    int distance = abs((int)note->pitch - previous->pitch);
    if (distance > 12 || note->channel != previous->channel) return 18.0;
    double old_duration = fmax(.020, previous->end - previous->time);
    double new_duration = fmax(.020, note->end - note->time);
    double cost = distance + 2.0 * fabs(log(new_duration / old_duration)) +
        2.0 * fmax(0.0, note->time - previous->end) / horizon;
    if (previous->end > note->time + .08 && new_duration < old_duration * .4)
        cost += 12.0;
    return cost;
}

/* Match complete onset groups, not "highest note = melody". Only recent,
   nearby, same-channel notes can belong to one voice. Held inner melodies
   can therefore coexist with short, higher ornament streams. */
static void human_track_voices(HumanNote *notes, const HumanGroup *groups,
                               size_t group_count, HumanVoice *voices, double beat) {
    size_t active[HUMAN_ACTIVE_VOICES], voice_count = 0;
    for (size_t a = 0; a < HUMAN_ACTIVE_VOICES; ++a) active[a] = SIZE_MAX;
    double horizon = human_clamp(3.0 * beat, 0.8, 2.0);
    /* Candidates' previous notes stay fixed throughout one onset group. Cache
       their exact costs once instead of repeating logarithms at every greedy
       assignment. The original row/slot traversal and strict tie rules remain.
       Reuse one bounded scratch block; allocation failure uses the old path. */
    size_t maximum = 0;
    for (size_t g = 0; g < group_count; ++g) {
        size_t width = groups[g].end - groups[g].begin;
        if (width > maximum) maximum = width;
    }
    double *costs = maximum > 1 && maximum <= SIZE_MAX / (HUMAN_ACTIVE_VOICES * sizeof(double))
        ? malloc(maximum * HUMAN_ACTIVE_VOICES * sizeof(double)) : NULL;
    for (size_t g = 0; g < group_count; ++g) {
        const HumanGroup *group = &groups[g];
        bool used[HUMAN_ACTIVE_VOICES] = {false};
        for (size_t a = 0; a < HUMAN_ACTIVE_VOICES; ++a) {
            if (active[a] != SIZE_MAX) {
                const HumanNote *last = &notes[voices[active[a]].last];
                if (group->time > fmax(last->time + horizon, last->end + .1)) active[a] = SIZE_MAX;
            }
        }
        bool cached = costs && group->end - group->begin > 1;
        if (cached) {
            for (size_t i = group->begin; i < group->end; ++i) {
                for (size_t a = 0; a < HUMAN_ACTIVE_VOICES; ++a) {
                    costs[(i - group->begin) * HUMAN_ACTIVE_VOICES + a] = active[a] == SIZE_MAX
                        ? 18.0 : human_voice_cost(&notes[i], &notes[voices[active[a]].last], horizon);
                }
            }
        }
        for (;;) {
            size_t best_note = SIZE_MAX, best_slot = SIZE_MAX;
            double best_cost = 18.0;
            for (size_t i = group->begin; i < group->end; ++i) {
                if (notes[i].voice != SIZE_MAX) continue;
                for (size_t a = 0; a < HUMAN_ACTIVE_VOICES; ++a) {
                    if (used[a] || active[a] == SIZE_MAX) continue;
                    double cost = cached ? costs[(i - group->begin) * HUMAN_ACTIVE_VOICES + a]
                        : human_voice_cost(&notes[i], &notes[voices[active[a]].last], horizon);
                    if (cost < best_cost) {
                        best_cost = cost; best_note = i; best_slot = a;
                    }
                }
            }
            if (best_note == SIZE_MAX) break;
            size_t v = active[best_slot], previous = voices[v].last;
            notes[best_note].voice = v; notes[best_note].previous = previous;
            notes[previous].next = best_note;
            used[best_slot] = true;
        }
        for (size_t i = group->begin; i < group->end; ++i) {
            if (notes[i].voice == SIZE_MAX) {
                size_t v = voice_count++;
                notes[i].voice = v;
                for (size_t a = 0; a < HUMAN_ACTIVE_VOICES; ++a) {
                    if (active[a] == SIZE_MAX) { active[a] = v; used[a] = true; break; }
                }
            }
            HumanVoice *voice = &voices[notes[i].voice];
            voice->last = i;
        }
    }
    free(costs);
    /* Local profiles, not a whole-song pitch/velocity average. A melody that
       changes register or hands can change role without rewriting its notes. */
    size_t note_count = groups[group_count - 1].end;
    for (size_t i = 0; i < note_count; ++i) {
        size_t samples[7], sample_count = 1; samples[0] = i;
        size_t p = i, n = i;
        for (size_t step = 0; step < 3; ++step) {
            if (p != SIZE_MAX) p = notes[p].previous;
            if (n != SIZE_MAX) n = notes[n].next;
            if (p != SIZE_MAX && notes[i].time - notes[p].time <= 2.0 * beat) samples[sample_count++] = p;
            if (n != SIZE_MAX && notes[n].time - notes[i].time <= 2.0 * beat) samples[sample_count++] = n;
        }
        for (size_t s = 0; s < sample_count; ++s) {
            const HumanNote *sample = &notes[samples[s]];
            notes[i].voice_pitch += sample->pitch;
            notes[i].voice_duration += sample->end - sample->time;
            notes[i].voice_velocity += sample->velocity;
        }
        notes[i].voice_pitch /= sample_count; notes[i].voice_duration /= sample_count;
        notes[i].voice_velocity /= sample_count; notes[i].voice_notes = sample_count;
    }
}

static double human_voice_score(const HumanNote *note, int low, int high,
                               double mean_velocity, double beat) {
    return .28 * human_clamp((note->voice_pitch - low) / fmax(12.0, high - low), 0.0, 1.0) +
           .40 * human_clamp(note->voice_duration / (1.2 * beat), 0.0, 1.0) +
           .18 * human_clamp((double)(note->voice_notes - 1) / 3.0, 0.0, 1.0) +
           .14 * human_clamp((note->voice_velocity - mean_velocity + 20.0) / 40.0, 0.0, 1.0);
}

static void human_find_melody(const HumanNote *notes, HumanGroup *groups,
                             size_t group_count, double beat) {
    for (size_t g = 0; g < group_count; ++g) {
        size_t first = g > 4 ? g - 4 : 0, last = g + 5 < group_count ? g + 5 : group_count;
        int low = 127, high = 0;
        double velocity = 0.0; size_t nearby_count = 0;
        for (size_t h = first; h < last; ++h) {
            if (fabs(groups[h].time - groups[g].time) > 2.0 * beat) continue;
            for (size_t i = groups[h].begin; i < groups[h].end; ++i) {
                if (notes[i].pitch < low) low = notes[i].pitch;
                if (notes[i].pitch > high) high = notes[i].pitch;
                velocity += notes[i].velocity; ++nearby_count;
            }
        }
        double best = -1.0, second = -1.0; size_t melody = SIZE_MAX;
        if (!nearby_count) continue;
        velocity /= nearby_count;
        /* Deduplicate candidates so repeated occurrences of the same runner-up
           voice do not create a false ambiguity. */
        size_t candidates[128]; size_t candidate_count = 0, melody_note = SIZE_MAX;
        for (size_t h = first; h < last; ++h) {
            if (fabs(groups[h].time - groups[g].time) > 2.0 * beat) continue;
            for (size_t i = groups[h].begin; i < groups[h].end; ++i) {
                size_t v = notes[i].voice, j = 0;
                while (j < candidate_count && notes[candidates[j]].voice != v) ++j;
                if (j < candidate_count) {
                    if (fabs(notes[i].time - groups[g].time) < fabs(notes[candidates[j]].time - groups[g].time))
                        candidates[j] = i;
                } else if (candidate_count < 128) candidates[candidate_count++] = i;
            }
        }
        for (size_t j = 0; j < candidate_count; ++j) {
            size_t i = candidates[j];
            double score = human_voice_score(&notes[i], low, high, velocity, beat);
            if (score > best) { second = best; best = score; melody = notes[i].voice; melody_note = i; }
            else if (score > second) second = score;
        }
        HumanGroup *group = &groups[g]; group->melody = melody;
        /* Ambiguous counterpoint gets little/no imposed foreground/background. */
        group->confidence = high - low >= 7 && second >= 0.0
            ? human_clamp((best - second - .015) / .16, 0.0, 1.0) : 0.0;
        group->confidence *= human_clamp((high - low - 7.0) / 17.0, 0.0, 1.0);
        if (melody == SIZE_MAX) continue;
        double melody_velocity = notes[melody_note].voice_velocity;
        double other_velocity = 0.0; size_t others = 0;
        for (size_t i = group->begin; i < group->end; ++i) {
            if (notes[i].voice == melody) continue;
            other_velocity += notes[i].velocity; ++others;
        }
        if (others) {
            /* Existing velocity contrast is evidence, not something to flatten. */
            group->balance = human_clamp(8.0 - (melody_velocity - other_velocity / others), 0.0, 10.0)
                             * .65 * group->confidence;
        }
    }
}

static void human_phrase_map(const HumanNote *notes, HumanGroup *groups,
                             size_t group_count, double tail, double beat,
                             double strength, uint32_t seed) {
    double sounding_end = groups[0].time;
    size_t phrase = 0, phrase_begin = 0;
    for (size_t g = 0; g < group_count; ++g) {
        if (g) {
            double silence = groups[g].time - sounding_end;
            double gap = groups[g].time - groups[g - 1].time;
            bool rest = silence > fmax(.12, beat * .35) && !groups[g - 1].sustained;
            /* Sparse, held cadences can divide a long passage. No regular
               four-bar delay is imposed on already expressive performances. */
            bool cadence = groups[g - 1].time - groups[phrase_begin].time > 8.0 * beat &&
                gap > 1.35 * beat && groups[g - 1].end - groups[g - 1].begin <= 3;
            if (rest || cadence) { ++phrase; phrase_begin = g; }
        }
        groups[g].phrase = phrase;
        for (size_t i = groups[g].begin; i < groups[g].end; ++i)
            sounding_end = fmax(sounding_end, notes[i].end);
    }
    for (size_t begin = 0; begin < group_count;) {
        size_t end = begin + 1;
        while (end < group_count && groups[end].phrase == groups[begin].phrase) ++end;
        double start = groups[begin].time, span = groups[end - 1].time - start;
        for (size_t g = begin; g < end; ++g) {
            double x = span > .1 ? (groups[g].time - start) / span : 0.0;
            double envelope = sin(HUMAN_PI * x);
            double gap = .5;
            if (g) gap = fmin(gap, groups[g].time - groups[g - 1].time);
            if (g + 1 < group_count) gap = fmin(gap, groups[g + 1].time - groups[g].time);
            else gap = fmin(gap, tail - groups[g].time);
            double budget = fmin(.030 * strength, fmax(0.0, gap) * .10);
            /* A small early push and slower cadence; the following phrase
               entry stays pinned so breathing never accumulates tempo drift. */
            double rubato = budget * (x * x - .6 * envelope);
            /* Correlated phrase motion, not independent per-onset noise.
               Both endpoints stay pinned and no delay accumulates. */
            double jitter = pn_human_noise(seed, notes[groups[begin].begin].id) *
                sin(2.0 * HUMAN_PI * x) * fmin(.002 * strength, gap * .015) * envelope;
            groups[g].mapped = groups[g].time + rubato + jitter;
            groups[g].shape = span > .1 ? .8 * envelope - .65 * x : 0.0;
        }
        begin = end;
    }
}

/* Strictly increasing map: controls are never independently randomized.
   Their order, values, channels and relation to phrases survive. */
static double human_map_time(double time, const HumanGroup *groups, size_t count) {
    if (time <= groups[0].time) return time;
    if (time >= groups[count - 1].time) {
        /* Carry the final cadence into its release/CCs instead of squeezing
           the last held note. Hard cutoffs are still applied by common.c. */
        return time + groups[count - 1].mapped - groups[count - 1].time;
    }
    size_t low = 0, high = count - 1;
    while (high - low > 1) {
        size_t middle = low + (high - low) / 2;
        if (groups[middle].time <= time) low = middle; else high = middle;
    }
    double mix = (time - groups[low].time) / (groups[high].time - groups[low].time);
    return groups[low].mapped + mix * (groups[high].mapped - groups[low].mapped);
}

static void human_chord_attacks(HumanNote *notes, const HumanGroup *groups,
                               size_t group_count, double strength, uint32_t seed) {
    for (size_t g = 0; g < group_count; ++g) {
        const HumanGroup *group = &groups[g];
        if (group->end - group->begin < 2) continue;
        double gap = .4;
        if (g) gap = fmin(gap, group->time - groups[g - 1].time);
        if (g + 1 < group_count) gap = fmin(gap, groups[g + 1].time - group->time);
        double span = fmin(.009 * strength, gap * .045);
        int low = 127, high = 0;
        for (size_t i = group->begin; i < group->end; ++i) {
            if (notes[i].pitch < low) low = notes[i].pitch;
            if (notes[i].pitch > high) high = notes[i].pitch;
        }
        double minimum = 1.0;
        for (size_t i = group->begin; i < group->end; ++i) {
            double position = (double)(notes[i].pitch - low) / fmax(1.0, high - low);
            double offset = span * (.2 + .6 * position);
            if (notes[i].voice == group->melody && group->confidence > .25) offset = 0.0;
            else if (notes[i].pitch == low) offset = span * .12;
            offset += fmin(.001 * strength, gap * .005) *
                (.5 + .5 * pn_human_noise(seed, notes[i].id + 19u));
            notes[i].attack_offset = offset;
            minimum = fmin(minimum, offset);
        }
        for (size_t i = group->begin; i < group->end; ++i) notes[i].attack_offset -= minimum;
    }
}

static int human_double_compare(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static bool human_release_candidate(const HumanNote *notes, size_t i) {
    const HumanNote *note = &notes[i];
    if (note->off == SIZE_MAX || note->end - note->time < .080) return false;
    /* Leave fast runs, trills and repeated fingers alone, including their
       final note. The written articulation takes priority over this detail. */
    if (note->previous != SIZE_MAX && note->time - notes[note->previous].time < .075)
        return false;
    if (note->next != SIZE_MAX && notes[note->next].time - note->time < .075)
        return false;
    return true;
}

static bool human_neighbor_releases(PnSong *song, HumanNote *notes,
                                    const HumanGroup *groups, size_t group_count,
                                    double strength, uint32_t seed, wchar_t *error, size_t cap) {
    /* Only shorten almost-equal, co-struck chord fingers within two octaves. Existing
       duration contrast is not flattened, attacks/velocities/pedals stay put,
       and the latest release of each chord remains an unmodified anchor. */
    size_t cc_count = 0;
    for (size_t i = 0; i < song->event_count; ++i) {
        const PnEvent *e = &song->events[i];
        if (e->type == PN_CONTROL_CHANGE &&
            (e->data1 == 64 || e->data1 == 66 || e->data1 == 67)) ++cc_count;
    }
    double *controls = cc_count ? malloc(cc_count * sizeof(*controls)) : NULL;
    if (cc_count && !controls) {
        pn_set_error(error, cap, L"Not enough memory for neighboring finger releases.");
        return false;
    }
    size_t c = 0;
    for (size_t i = 0; i < song->event_count; ++i) {
        const PnEvent *e = &song->events[i];
        if (e->type == PN_CONTROL_CHANGE &&
            (e->data1 == 64 || e->data1 == 66 || e->data1 == 67)) controls[c++] = e->time;
    }
    if (cc_count) qsort(controls, cc_count, sizeof(*controls), human_double_compare);
    for (size_t g = 0; g < group_count; ++g) {
        const HumanGroup *group = &groups[g];
        size_t anchor = SIZE_MAX;
        for (size_t i = group->begin; i < group->end; ++i) {
            HumanNote *n = &notes[i];
            if (n->off == SIZE_MAX) continue;
            double budget = fmin(.012, fmin(.010 * strength, (n->end - n->time) * .04));
            n->release_floor = fmax(song->events[n->on].time + .001,
                                    song->events[n->off].time - budget);
            if (anchor == SIZE_MAX || song->events[n->off].time > song->events[notes[anchor].off].time)
                anchor = i;
        }
        for (size_t i = group->begin; i < group->end; ++i) {
            if (!human_release_candidate(notes, i)) continue;
            for (size_t j = i + 1; j < group->end; ++j) {
                if (!human_release_candidate(notes, j)) continue;
                int distance = abs((int)notes[i].pitch - notes[j].pitch);
                double written = fmin(notes[i].end - notes[i].time, notes[j].end - notes[j].time);
                if (distance < 1 || distance > 24 || notes[i].channel != notes[j].channel ||
                    fabs(notes[i].time - notes[j].time) > .001 ||
                    fabs((notes[i].end - notes[i].time) - (notes[j].end - notes[j].time)) > fmin(.008, written * .03))
                    continue;
                double a = song->events[notes[i].off].time - song->events[notes[i].on].time;
                double b = song->events[notes[j].off].time - song->events[notes[j].on].time;
                uint32_t finger_pair = notes[i].id ^ (notes[j].id * 0x9e3779b9u) ^ 0x7f4a7c15u;
                double contrast = fmin(.006 * strength, written * .025) *
                    (.8 + .2 * pn_human_noise(seed, finger_pair));
                if (fabs(a - b) >= contrast) continue; /* already differentiated */
                size_t keep = a >= b ? i : j;
                if (i == anchor || j == anchor) keep = anchor;
                else if (group->confidence > .25) {
                    if (notes[i].voice == group->melody) keep = i;
                    else if (notes[j].voice == group->melody) keep = j;
                }
                size_t trim = keep == i ? j : i;
                double held = song->events[notes[keep].off].time - song->events[notes[keep].on].time;
                PnEvent *off = &song->events[notes[trim].off];
                double candidate = fmax(notes[trim].release_floor,
                    song->events[notes[trim].on].time + held - contrast);
                /* Do not move an off across an original or inferred pedal
                   transition. Controls themselves are never rewritten here. */
                size_t lo = 0, hi = cc_count;
                while (lo < hi) {
                    size_t mid = lo + (hi - lo) / 2;
                    if (controls[mid] < off->time - 1e-9) lo = mid + 1; else hi = mid;
                }
                if (lo && controls[lo - 1] >= candidate)
                    candidate = fmax(candidate, controls[lo - 1] + .001);
                off->time = fmin(off->time, candidate);
            }
        }
    }
    free(controls);
    return true;
}

static bool human_add_pedal(PnSong *song, const HumanNote *notes,
                           const HumanGroup *groups, size_t group_count,
                           const PnOptions *options, double beat,
                           bool has_pedal, wchar_t *error, size_t cap) {
    /* No invented pedal for transcribed performances, existing CC64, Roblox,
       subtle mode, solos, short/staccato chords or ambiguous counterpoint. */
    if (!options->backend_midi || options->humanize < 2 || song->performance_source || has_pedal)
        return true;
    uint32_t order = 0;
    for (size_t i = 0; i < song->event_count; ++i)
        if (song->events[i].order > order) order = song->events[i].order;
    if ((uint64_t)order + 2 * (uint64_t)group_count > UINT32_MAX) return true;
    double previous_up = -1.0;
    for (size_t g = 0; g + 1 < group_count; ++g) {
        const HumanGroup *group = &groups[g];
        double gap = groups[g + 1].time - group->time;
        if (group->end - group->begin < 3 || gap < .18 || gap > 1.5 * beat ||
            group->confidence < .25 || group->phrase != groups[g + 1].phrase) continue;
        unsigned pitch_classes = 0, class_count = 0;
        double earliest_off = 1e30, latest_off = 0.0, latest_on = 0.0, gate_sum = 0.0;
        uint8_t channel = notes[group->begin].channel;
        bool compatible = true;
        for (size_t i = group->begin; i < group->end; ++i) {
            if (notes[i].off == SIZE_MAX || notes[i].channel != channel ||
                notes[i].end - notes[i].time < .55 * gap) { compatible = false; continue; }
            pitch_classes |= 1u << (notes[i].pitch % 12);
            const PnEvent *on = &song->events[notes[i].on], *off = &song->events[notes[i].off];
            earliest_off = fmin(earliest_off, off->time);
            latest_off = fmax(latest_off, off->time);
            latest_on = fmax(latest_on, on->time);
            gate_sum += notes[i].end - notes[i].time;
        }
        for (unsigned bits = pitch_classes; bits; bits >>= 1) class_count += bits & 1u;
        if (!compatible || class_count < 3 || gate_sum / (group->end - group->begin) < .72 * gap)
            continue;
        double down = fmax(latest_on + .018, previous_up + .006);
        /* A tiny repedal overlap is safe only if the next sustained chord has
           exactly the same pitch classes AND source notes already overlap it.
           A new harmony, staccato or actual rest keeps the early lift. Existing
           source CC64/performance files never enter this inference path. */
        const HumanGroup *next = &groups[g + 1];
        unsigned next_classes = 0;
        double next_on = 1e30;
        bool next_held = next->end - next->begin >= 3;
        for (size_t i = next->begin; i < next->end; ++i) {
            next_classes |= 1u << (notes[i].pitch % 12);
            if (notes[i].off == SIZE_MAX || notes[i].channel != channel ||
                notes[i].end - notes[i].time < .55 * gap) next_held = false;
            next_on = fmin(next_on, song->events[notes[i].on].time);
        }
        double overlap = next_held && next_classes == pitch_classes && latest_off >= next_on + .008
            ? .008 : -.012;
        double up = fmin(next_on + overlap, fmin(latest_off + .040, down + 1.2));
        if (down >= earliest_off - .004 || up - down < .08) continue;
        PnEvent event = {0};
        event.time = down; event.order = ++order; event.type = PN_CONTROL_CHANGE;
        event.channel = channel; event.data1 = 64; event.data2 = 127;
        if (!pn_song_add_event(song, event, error, cap)) return false;
        event.time = up; event.order = ++order; event.data2 = 0;
        if (!pn_song_add_event(song, event, error, cap)) return false;
        previous_up = up;
    }
    return true;
}

bool pn_humanize_song(PnSong *song, const PnOptions *options,
                     wchar_t *error, size_t cap) {
    double strength = pn_human_strength(song, options);
    if (!strength) return true;
    size_t count = 0; uint32_t max_id = 0; double tail = 0.0;
    bool has_pedal = false;
    for (size_t i = 0; i < song->event_count; ++i) {
        PnEvent e = song->events[i];
        tail = fmax(tail, e.time);
        if (e.type == PN_CONTROL_CHANGE && e.data1 == 64) has_pedal = true;
        if (e.type != PN_NOTE_ON) continue;
        ++count; if (e.note_id > max_id) max_id = e.note_id;
    }
    if (!count) return true;
    if ((size_t)max_id + 1 > SIZE_MAX / sizeof(size_t) ||
        count > SIZE_MAX / sizeof(HumanNote) || count > SIZE_MAX / sizeof(HumanGroup) ||
        count > SIZE_MAX / sizeof(HumanVoice)) {
        pn_set_error(error, cap, L"Human performance analysis is too large."); return false;
    }
    HumanNote *notes = calloc(count, sizeof(*notes));
    HumanGroup *groups = calloc(count, sizeof(*groups));
    HumanVoice *voices = calloc(count, sizeof(*voices));
    size_t *by_id = malloc(((size_t)max_id + 1) * sizeof(*by_id));
    if (!notes || !groups || !voices || !by_id) {
        free(notes); free(groups); free(voices); free(by_id);
        pn_set_error(error, cap, L"Not enough memory for musical expression."); return false;
    }
    for (size_t i = 0; i <= max_id; ++i) by_id[i] = SIZE_MAX;
    size_t n = 0;
    for (size_t i = 0; i < song->event_count; ++i) {
        PnEvent e = song->events[i];
        if (e.type == PN_NOTE_ON) {
            HumanNote *note = &notes[n];
            note->on = i; note->off = note->previous = note->next = note->voice = SIZE_MAX;
            note->time = e.time; note->end = e.time + .020; note->id = e.note_id;
            note->pitch = e.data1; note->channel = e.channel; note->velocity = e.data2;
            by_id[e.note_id] = n++;
        } else if (e.type == PN_NOTE_OFF && e.note_id <= max_id && by_id[e.note_id] != SIZE_MAX) {
            notes[by_id[e.note_id]].off = i; notes[by_id[e.note_id]].end = e.time;
        }
    }
    free(by_id);
    size_t group_count = 0;
    for (size_t begin = 0; begin < count;) {
        size_t end = begin + 1;
        while (end < count && notes[end].time - notes[begin].time <= .001) ++end;
        HumanGroup *group = &groups[group_count];
        group->begin = begin; group->end = end; group->time = notes[begin].time;
        group->melody = SIZE_MAX;
        for (size_t i = begin; i < end; ++i) notes[i].group = group_count;
        ++group_count; begin = end;
    }
    bool pedal[16] = {false}; size_t cursor = 0;
    for (size_t g = 0; g < group_count; ++g) {
        while (cursor < song->event_count && song->events[cursor].time <= groups[g].time) {
            const PnEvent *e = &song->events[cursor++];
            if (e->type == PN_CONTROL_CHANGE && e->data1 == 64 && e->channel < 16)
                pedal[e->channel] = e->data2 >= 64;
        }
        for (size_t ch = 0; ch < 16; ++ch) groups[g].sustained |= pedal[ch];
    }
    double beat = human_clamp(60.0 / fmax(20.0, song->bpm) / options->speed, .12, 2.0);
    human_track_voices(notes, groups, group_count, voices, beat);
    human_find_melody(notes, groups, group_count, beat);
    human_phrase_map(notes, groups, group_count, tail, beat, strength, options->human_seed);
    human_chord_attacks(notes, groups, group_count, strength, options->human_seed);
    for (size_t i = 0; i < song->event_count; ++i)
        song->events[i].time = human_map_time(song->events[i].time, groups, group_count);
    for (size_t i = 0; i < count; ++i) {
        HumanNote *note = &notes[i]; const HumanGroup *group = &groups[note->group];
        PnEvent *on = &song->events[note->on];
        on->time += note->attack_offset;
        double duration = note->end - note->time;
        double foreground = note->voice == group->melody ? .65 : -.35;
        double dynamics = foreground * group->balance + 4.0 * group->shape +
            .6 * pn_human_noise(options->human_seed, note->id + 17u);
        /* Don't override strong/soft source contrasts with inferred roles. */
        double protect = song->performance_source && (note->velocity < 40 || note->velocity > 100) ? .35 : 1.0;
        int velocity = (int)lround(note->velocity + strength * protect * dynamics);
        on->data2 = (uint8_t)human_clamp(velocity, 1.0, 127.0);
        if (note->off == SIZE_MAX) continue;
        PnEvent *off = &song->events[note->off];
        double mapped_end = off->time + note->attack_offset;
        double release = pn_human_noise(options->human_seed, note->id + 73u) *
            fmin(.005 * strength, duration * .025 * strength);
        if (note->next != SIZE_MAX) {
            const HumanNote *next = &notes[note->next];
            const HumanGroup *next_group = &groups[next->group];
            double interval = next->time - note->time;
            double ratio = duration / fmax(.001, interval);
            if (ratio < .60) {
                release = -fmin(.006 * strength, duration * .04 * strength) *
                    (.5 + .5 * pn_human_noise(options->human_seed, note->id + 91u));
            } else if (ratio >= .80 && ratio <= 1.10 && interval >= .075 &&
                       group->phrase == next_group->phrase &&
                       fabs(next->time - note->end) <= fmin(.055, interval * .12)) {
                double next_attack = human_map_time(next->time, groups, group_count) + next->attack_offset;
                double target = next_attack + (note->pitch == next->pitch ? -.004 : .006);
                double budget = fmin(.028 * strength, fmin(duration * .06, interval * .07));
                release = human_clamp(target - mapped_end, -budget, budget);
            }
        }
        if (note->group + 1 < group_count && groups[note->group + 1].phrase != group->phrase &&
            note->end < groups[note->group + 1].time) {
            release = fmin(release, 0.0); /* Do not fill the written breath. */
        }
        off->time = fmax(on->time + .001, mapped_end + release);
    }
    bool ok = human_add_pedal(song, notes, groups, group_count, options, beat, has_pedal, error, cap);
    if (ok) ok = human_neighbor_releases(song, notes, groups, group_count, strength,
                                        options->human_seed, error, cap);
    free(notes); free(groups); free(voices);
    pn_song_sort(song);
    return ok;
}
