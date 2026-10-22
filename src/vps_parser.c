#include "piano_native.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct VpsNote {
    int midi;
    double start;
    double duration;
    int bar;
} VpsNote;

typedef struct VpsNotes {
    VpsNote *items;
    size_t count;
    size_t capacity;
} VpsNotes;

typedef struct VpsTempo {
    int bar;
    double beat;
    double bpm;
} VpsTempo;

typedef struct VpsTempos {
    VpsTempo *items;
    size_t count;
    size_t capacity;
} VpsTempos;

typedef struct VpsBarOverride {
    int bar;
    double length;
} VpsBarOverride;

typedef struct VpsOverrides {
    VpsBarOverride *items;
    size_t count;
    size_t capacity;
} VpsOverrides;

typedef struct VpsVoice {
    char name[64];
    double cursor;
    double last_duration;
    double bar_start;
    int bar;
    char *previous_token;
} VpsVoice;

typedef struct VpsVoices {
    VpsVoice *items;
    size_t count;
    size_t capacity;
} VpsVoices;

typedef struct TokenVec {
    char **items;
    size_t count;
    size_t capacity;
} TokenVec;

static char *vps_strdup(const char *text) {
    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1);
    if (copy) memcpy(copy, text, length + 1);
    return copy;
}

static char *vps_trim(char *text) {
    while (*text && isspace((unsigned char)*text)) ++text;
    size_t length = strlen(text);
    while (length && isspace((unsigned char)text[length - 1])) text[--length] = '\0';
    return text;
}

static void vps_strip_comment(char *line) {
    for (size_t index = 0; line[index]; ++index) {
        bool boundary = index == 0 || isspace((unsigned char)line[index - 1]);
        if (boundary && line[index] == '#') {
            line[index] = '\0';
            return;
        }
        if (boundary && line[index] == '/' && line[index + 1] == '/') {
            line[index] = '\0';
            return;
        }
    }
}

static bool vps_token_add(TokenVec *vec, const char *start, size_t length,
                          wchar_t *error, size_t cap) {
    if (vec->count == vec->capacity) {
        size_t next = vec->capacity ? vec->capacity * 2 : 64;
        char **resized = (char **)realloc(vec->items, next * sizeof(char *));
        if (!resized) {
            pn_set_error(error, cap, L"Not enough memory for VPS tokens.");
            return false;
        }
        vec->items = resized;
        vec->capacity = next;
    }
    char *token = (char *)malloc(length + 1);
    if (!token) {
        pn_set_error(error, cap, L"Not enough memory for a VPS token.");
        return false;
    }
    memcpy(token, start, length);
    token[length] = '\0';
    vec->items[vec->count++] = token;
    return true;
}

static void vps_tokens_free(TokenVec *vec) {
    for (size_t index = 0; index < vec->count; ++index) free(vec->items[index]);
    free(vec->items);
    memset(vec, 0, sizeof(*vec));
}

static bool vps_scan_tokens(const char *body, TokenVec *tokens,
                            wchar_t *error, size_t cap) {
    const char *cursor = body;
    while (*cursor) {
        while (*cursor && isspace((unsigned char)*cursor)) ++cursor;
        if (!*cursor) break;
        if (*cursor == '{') {
            if (!vps_token_add(tokens, cursor++, 1, error, cap)) return false;
            continue;
        }
        if (*cursor == '}') {
            const char *start = cursor++;
            if (*cursor == '*') {
                ++cursor;
                while (isdigit((unsigned char)*cursor)) ++cursor;
            }
            if (!vps_token_add(tokens, start, (size_t)(cursor - start), error, cap)) return false;
            continue;
        }
        const char *start = cursor;
        if (*cursor == '[') {
            const char *close = strchr(cursor, ']');
            if (!close) {
                pn_set_error(error, cap, L"VPS chord is missing ']'.");
                return false;
            }
            cursor = close + 1;
            while (*cursor && !isspace((unsigned char)*cursor) &&
                   *cursor != '{' && *cursor != '}') ++cursor;
        } else {
            while (*cursor && !isspace((unsigned char)*cursor) &&
                   *cursor != '{' && *cursor != '}') ++cursor;
        }
        if (cursor > start && !vps_token_add(
                tokens, start, (size_t)(cursor - start), error, cap)) return false;
    }
    return true;
}

static bool vps_expand_groups(const TokenVec *input, TokenVec *output,
                              wchar_t *error, size_t cap) {
    TokenVec stack[32] = {0};
    int depth = 0;
    for (size_t index = 0; index < input->count; ++index) {
        const char *token = input->items[index];
        if (strcmp(token, "{") == 0) {
            if (depth >= 32) {
                pn_set_error(error, cap, L"VPS group nesting is too deep.");
                goto fail;
            }
            ++depth;
            continue;
        }
        if (token[0] == '}') {
            if (depth <= 0) {
                pn_set_error(error, cap, L"VPS group has an unmatched '}'.");
                goto fail;
            }
            int repeat = 1;
            if (token[1] == '*') repeat = atoi(token + 2);
            if (repeat < 1 || repeat > 10000) {
                pn_set_error(error, cap, L"Invalid VPS group repeat count.");
                goto fail;
            }
            TokenVec *body = &stack[depth - 1];
            TokenVec *target = depth > 1 ? &stack[depth - 2] : output;
            for (int copy = 0; copy < repeat; ++copy) {
                for (size_t item = 0; item < body->count; ++item) {
                    if (!vps_token_add(target, body->items[item],
                                       strlen(body->items[item]), error, cap)) goto fail;
                }
            }
            vps_tokens_free(body);
            --depth;
            continue;
        }
        TokenVec *target = depth ? &stack[depth - 1] : output;
        if (!vps_token_add(target, token, strlen(token), error, cap)) goto fail;
    }
    if (depth != 0) {
        pn_set_error(error, cap, L"VPS group has an unmatched '{'.");
        goto fail;
    }
    return true;

fail:
    for (int index = 0; index < 32; ++index) vps_tokens_free(&stack[index]);
    return false;
}

static bool vps_notes_add(VpsNotes *notes, VpsNote note,
                          wchar_t *error, size_t cap) {
    if (notes->count == notes->capacity) {
        size_t next = notes->capacity ? notes->capacity * 2 : 4096;
        VpsNote *resized = (VpsNote *)realloc(notes->items, next * sizeof(VpsNote));
        if (!resized) {
            pn_set_error(error, cap, L"Not enough memory for VPS notes.");
            return false;
        }
        notes->items = resized;
        notes->capacity = next;
    }
    notes->items[notes->count++] = note;
    return true;
}

static bool vps_tempos_add(VpsTempos *tempos, VpsTempo tempo,
                           wchar_t *error, size_t cap) {
    if (tempos->count == tempos->capacity) {
        size_t next = tempos->capacity ? tempos->capacity * 2 : 64;
        VpsTempo *resized = (VpsTempo *)realloc(tempos->items, next * sizeof(VpsTempo));
        if (!resized) {
            pn_set_error(error, cap, L"Not enough memory for VPS tempos.");
            return false;
        }
        tempos->items = resized;
        tempos->capacity = next;
    }
    tempos->items[tempos->count++] = tempo;
    return true;
}

static bool vps_overrides_add(VpsOverrides *items, VpsBarOverride value,
                              wchar_t *error, size_t cap) {
    for (size_t index = 0; index < items->count; ++index) {
        if (items->items[index].bar == value.bar) {
            items->items[index] = value;
            return true;
        }
    }
    if (items->count == items->capacity) {
        size_t next = items->capacity ? items->capacity * 2 : 32;
        VpsBarOverride *resized = (VpsBarOverride *)realloc(
            items->items, next * sizeof(VpsBarOverride)
        );
        if (!resized) {
            pn_set_error(error, cap, L"Not enough memory for VPS bar lengths.");
            return false;
        }
        items->items = resized;
        items->capacity = next;
    }
    items->items[items->count++] = value;
    return true;
}

static VpsVoice *vps_get_voice(VpsVoices *voices, const char *name,
                               double pickup, wchar_t *error, size_t cap) {
    for (size_t index = 0; index < voices->count; ++index) {
        if (strcmp(voices->items[index].name, name) == 0) return &voices->items[index];
    }
    if (voices->count == voices->capacity) {
        size_t next = voices->capacity ? voices->capacity * 2 : 8;
        VpsVoice *resized = (VpsVoice *)realloc(voices->items, next * sizeof(VpsVoice));
        if (!resized) {
            pn_set_error(error, cap, L"Not enough memory for VPS voices.");
            return NULL;
        }
        voices->items = resized;
        voices->capacity = next;
    }
    VpsVoice *voice = &voices->items[voices->count++];
    memset(voice, 0, sizeof(*voice));
    strncpy_s(voice->name, sizeof(voice->name), name, _TRUNCATE);
    voice->last_duration = 1.0;
    voice->bar = pickup > 0.0 ? 0 : 1;
    return voice;
}

static double vps_duration(const char *text, int divisions, bool *ok) {
    *ok = false;
    if (!text || !*text) return 0.0;
    size_t length = strlen(text);
    if (length > 1 && text[length - 1] == 'd') {
        char *end;
        long value = strtol(text, &end, 10);
        if (end == text + length - 1 && divisions > 0) {
            *ok = true;
            return (double)value / divisions;
        }
        return 0.0;
    }
    char *number_end;
    double numeric = strtod(text, &number_end);
    if (number_end != text && *number_end == '\0') {
        *ok = numeric > 0.0;
        return numeric;
    }
    double base;
    switch (text[0]) {
        case 'w': base = 4.0; break;
        case 'h': base = 2.0; break;
        case 'q': base = 1.0; break;
        case 'e': base = 0.5; break;
        case 's': base = 0.25; break;
        case 't': base = 0.125; break;
        case 'x': base = 0.0625; break;
        default: return 0.0;
    }
    const char *cursor = text + 1;
    int dots = 0;
    while (*cursor == '.') {
        ++dots;
        ++cursor;
    }
    if (dots) base *= 2.0 - pow(2.0, -dots);
    if (*cursor) {
        char *end;
        long tuple = strtol(cursor, &end, 10);
        if (*end || tuple < 2) return 0.0;
        long power = 1;
        while (power * 2 <= tuple) power *= 2;
        base *= (double)power / tuple;
    }
    *ok = true;
    return base;
}

static int vps_parse_note(const char *text, bool *ok) {
    *ok = false;
    if (!text || !strchr("ABCDEFGabcdefg", text[0])) return 0;
    int pitch_class;
    switch ((char)toupper((unsigned char)text[0])) {
        case 'C': pitch_class = 0; break;
        case 'D': pitch_class = 2; break;
        case 'E': pitch_class = 4; break;
        case 'F': pitch_class = 5; break;
        case 'G': pitch_class = 7; break;
        case 'A': pitch_class = 9; break;
        default: pitch_class = 11; break;
    }
    const char *cursor = text + 1;
    while (*cursor && !isdigit((unsigned char)*cursor) && *cursor != '-') {
        if (*cursor == '#' || *cursor == 's') ++pitch_class;
        else if (*cursor == 'b' || *cursor == 'f') --pitch_class;
        else if (*cursor == 'x' || *cursor == 'X') pitch_class += 2;
        else if ((unsigned char)cursor[0] == 0xE2 &&
                 (unsigned char)cursor[1] == 0x99 &&
                 (unsigned char)cursor[2] == 0xAF) {
            ++pitch_class;
            cursor += 2;
        } else if ((unsigned char)cursor[0] == 0xE2 &&
                   (unsigned char)cursor[1] == 0x99 &&
                   (unsigned char)cursor[2] == 0xAD) {
            --pitch_class;
            cursor += 2;
        } else return 0;
        ++cursor;
    }
    if (!*cursor) return 0;
    char *end;
    long octave = strtol(cursor, &end, 10);
    if (*end) return 0;
    int midi = (int)((octave + 1) * 12 + pitch_class);
    *ok = true;
    return midi;
}

static double vps_bar_length(int bar, int num, int den,
                             const VpsOverrides *overrides) {
    for (size_t index = 0; index < overrides->count; ++index) {
        if (overrides->items[index].bar == bar) return overrides->items[index].length;
    }
    return num * (4.0 / den);
}

static double vps_bar_start(int bar, double pickup, int num, int den,
                            const VpsOverrides *overrides) {
    if (bar <= 0) return 0.0;
    double beat = pickup;
    for (int current = 1; current < bar; ++current) {
        beat += vps_bar_length(current, num, den, overrides);
    }
    return beat;
}

static int vps_tempo_compare(const void *left_ptr, const void *right_ptr) {
    const VpsTempo *left = (const VpsTempo *)left_ptr;
    const VpsTempo *right = (const VpsTempo *)right_ptr;
    if (left->beat < right->beat) return -1;
    if (left->beat > right->beat) return 1;
    return 0;
}

static double vps_beat_seconds(double beat, double initial_bpm,
                               const VpsTempos *tempos) {
    double seconds = 0.0;
    double cursor = 0.0;
    double bpm = initial_bpm;
    for (size_t index = 0; index < tempos->count; ++index) {
        VpsTempo point = tempos->items[index];
        if (point.beat >= beat) break;
        seconds += (point.beat - cursor) * 60.0 / bpm;
        cursor = point.beat;
        bpm = point.bpm;
    }
    return seconds + (beat - cursor) * 60.0 / bpm;
}

static bool vps_emit_token(const char *token, VpsVoice *voice,
                           VpsNotes *notes, int divisions, int transpose,
                           wchar_t *error, size_t cap, int line_number) {
    char *copy = vps_strdup(token);
    if (!copy) {
        pn_set_error(error, cap, L"Not enough memory on VPS line %d.", line_number);
        return false;
    }
    char *duration_text = NULL;
    char *body = copy;
    if (body[0] == '[') {
        char *close = strchr(body, ']');
        if (!close) {
            free(copy);
            pn_set_error(error, cap, L"Unclosed chord on VPS line %d.", line_number);
            return false;
        }
        *close = '\0';
        body += 1;
        if (close[1]) {
            if (close[1] != '/') {
                free(copy);
                pn_set_error(error, cap, L"Invalid chord suffix on VPS line %d.", line_number);
                return false;
            }
            duration_text = close + 2;
        }
    } else {
        char *slash = strchr(body, '/');
        if (slash) {
            *slash = '\0';
            duration_text = slash + 1;
        }
    }

    double duration = voice->last_duration;
    if (duration_text) {
        bool duration_ok;
        duration = vps_duration(duration_text, divisions, &duration_ok);
        if (!duration_ok) {
            free(copy);
            pn_set_error(error, cap, L"Invalid duration on VPS line %d: %hs", line_number, duration_text);
            return false;
        }
    }
    voice->last_duration = duration;
    bool rest = _stricmp(body, "r") == 0 || strcmp(body, "-") == 0 ||
                _stricmp(body, "rest") == 0;
    if (!rest) {
        char *context = NULL;
        char *pitch = strtok_s(body, " ,\t", &context);
        if (!pitch) {
            free(copy);
            pn_set_error(error, cap, L"Empty chord on VPS line %d.", line_number);
            return false;
        }
        while (pitch) {
            bool note_ok;
            int midi = vps_parse_note(pitch, &note_ok) + transpose;
            if (!note_ok || midi < 0 || midi > 127 ||
                !vps_notes_add(notes, (VpsNote){midi, voice->cursor, duration, voice->bar},
                               error, cap)) {
                if (!note_ok) pn_set_error(error, cap, L"Invalid note on VPS line %d: %hs", line_number, pitch);
                else if (midi < 0 || midi > 127) pn_set_error(error, cap, L"Transposed note is outside MIDI range on line %d.", line_number);
                free(copy);
                return false;
            }
            pitch = strtok_s(NULL, " ,\t", &context);
        }
    }
    voice->cursor += duration;
    free(copy);
    return true;
}

static bool vps_read_file(const wchar_t *path, char **text,
                          wchar_t *error, size_t cap) {
    FILE *file = _wfopen(path, L"rb");
    if (!file) {
        pn_set_error(error, cap, L"Cannot open VPS file: %ls", path);
        return false;
    }
    _fseeki64(file, 0, SEEK_END);
    __int64 length = _ftelli64(file);
    rewind(file);
    if (length < 0 || (uint64_t)length > SIZE_MAX - 1) {
        fclose(file);
        pn_set_error(error, cap, L"VPS file is too large.");
        return false;
    }
    char *buffer = (char *)malloc((size_t)length + 1);
    if (!buffer) {
        fclose(file);
        pn_set_error(error, cap, L"Not enough memory to read VPS.");
        return false;
    }
    size_t read = fread(buffer, 1, (size_t)length, file);
    fclose(file);
    if (read != (size_t)length) {
        free(buffer);
        pn_set_error(error, cap, L"VPS file could not be read completely.");
        return false;
    }
    buffer[read] = '\0';
    *text = buffer;
    return true;
}

bool pn_load_vps(const wchar_t *path, PnSong *song, wchar_t *error, size_t cap) {
    pn_song_init(song);
    char *file_text = NULL;
    VpsNotes notes = {0};
    VpsTempos tempos = {0};
    VpsOverrides overrides = {0};
    VpsVoices voices = {0};
    double pickup = 0.0;
    int divisions = 480;
    int transpose = 0;
    bool success = false;

    if (!vps_read_file(path, &file_text, error, cap)) goto cleanup;
    char *text = file_text;
    if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) text += 3;

    char *line_context = NULL;
    char *line = strtok_s(text, "\r\n", &line_context);
    int line_number = 0;
    while (line) {
        ++line_number;
        vps_strip_comment(line);
        char *trimmed = vps_trim(line);
        if (!*trimmed) {
            line = strtok_s(NULL, "\r\n", &line_context);
            continue;
        }
        if (*trimmed == '@') {
            char *name = trimmed + 1;
            char *value = name;
            while (*value && !isspace((unsigned char)*value)) ++value;
            if (*value) *value++ = '\0';
            value = vps_trim(value);
            if (_stricmp(name, "title") == 0) {
                wchar_t wide[PN_TITLE_CAP];
                if (pn_utf8_to_wide(value, strlen(value), wide, PN_TITLE_CAP))
                    pn_copy_wide(song->title, PN_TITLE_CAP, wide);
            } else if (_stricmp(name, "bpm") == 0) {
                song->bpm = strtod(value, NULL);
            } else if (_stricmp(name, "meter") == 0) {
                char *slash = strchr(value, '/');
                if (!slash) {
                    pn_set_error(error, cap, L"Invalid @meter on VPS line %d.", line_number);
                    goto cleanup;
                }
                *slash = '\0';
                song->meter_num = atoi(value);
                song->meter_den = atoi(slash + 1);
            } else if (_stricmp(name, "divisions") == 0) {
                divisions = atoi(value);
                song->division = divisions;
            } else if (_stricmp(name, "transpose") == 0) {
                transpose = atoi(value);
            } else if (_stricmp(name, "pickup") == 0) {
                bool ok;
                pickup = vps_duration(value, divisions, &ok);
                if (!ok) {
                    pn_set_error(error, cap, L"Invalid @pickup on VPS line %d.", line_number);
                    goto cleanup;
                }
            } else if (_stricmp(name, "barlen") == 0) {
                char *space = value;
                int bar = (int)strtol(value, &space, 10);
                while (*space && isspace((unsigned char)*space)) ++space;
                bool ok;
                double length = vps_duration(space, divisions, &ok);
                if (!ok || bar < 1 || !vps_overrides_add(
                        &overrides, (VpsBarOverride){bar, length}, error, cap)) goto cleanup;
            } else if (_stricmp(name, "tempo") == 0) {
                char *space = value;
                int bar = (int)strtol(value, &space, 10);
                while (*space && isspace((unsigned char)*space)) ++space;
                double bpm = strtod(space, NULL);
                if (bar < 0 || bpm <= 0.0 || !vps_tempos_add(
                        &tempos, (VpsTempo){bar, 0.0, bpm}, error, cap)) goto cleanup;
            }
            line = strtok_s(NULL, "\r\n", &line_context);
            continue;
        }

        char *colon = strchr(trimmed, ':');
        if (!colon) {
            pn_set_error(error, cap, L"VPS line %d has no voice name.", line_number);
            goto cleanup;
        }
        *colon = '\0';
        char *voice_name = vps_trim(trimmed);
        for (char *cursor = voice_name; *cursor; ++cursor) {
            if (!isalnum((unsigned char)*cursor) && *cursor != '_') {
                pn_set_error(error, cap, L"Invalid voice name on VPS line %d.", line_number);
                goto cleanup;
            }
        }
        VpsVoice *voice = vps_get_voice(&voices, voice_name, pickup, error, cap);
        if (!voice) goto cleanup;
        TokenVec scanned = {0}, expanded = {0};
        if (!vps_scan_tokens(colon + 1, &scanned, error, cap) ||
            !vps_expand_groups(&scanned, &expanded, error, cap)) {
            vps_tokens_free(&scanned);
            vps_tokens_free(&expanded);
            goto cleanup;
        }
        for (size_t index = 0; index < expanded.count; ++index) {
            const char *token = expanded.items[index];
            if (strcmp(token, "|") == 0 || strcmp(token, "||") == 0 ||
                strcmp(token, "|:") == 0 || strcmp(token, ":|") == 0) {
                if (voice->cursor > voice->bar_start + 1e-9) {
                    voice->bar_start = voice->cursor;
                    ++voice->bar;
                }
                free(voice->previous_token);
                voice->previous_token = NULL;
                continue;
            }
            if (token[0] == '*' && isdigit((unsigned char)token[1])) {
                int repeat = atoi(token + 1);
                if (!voice->previous_token || repeat < 1 || repeat > 10000) {
                    pn_set_error(error, cap, L"Invalid token repeat on VPS line %d.", line_number);
                    vps_tokens_free(&scanned);
                    vps_tokens_free(&expanded);
                    goto cleanup;
                }
                for (int copy = 1; copy < repeat; ++copy) {
                    if (!vps_emit_token(voice->previous_token, voice, &notes,
                                        divisions, transpose, error, cap, line_number)) {
                        vps_tokens_free(&scanned);
                        vps_tokens_free(&expanded);
                        goto cleanup;
                    }
                }
                continue;
            }
            if (!vps_emit_token(token, voice, &notes, divisions, transpose,
                                error, cap, line_number)) {
                vps_tokens_free(&scanned);
                vps_tokens_free(&expanded);
                goto cleanup;
            }
            free(voice->previous_token);
            voice->previous_token = vps_strdup(token);
            if (!voice->previous_token) {
                pn_set_error(error, cap, L"Not enough memory for VPS repeat state.");
                vps_tokens_free(&scanned);
                vps_tokens_free(&expanded);
                goto cleanup;
            }
        }
        vps_tokens_free(&scanned);
        vps_tokens_free(&expanded);
        line = strtok_s(NULL, "\r\n", &line_context);
    }

    if (song->bpm <= 0.0 || song->meter_num <= 0 || song->meter_den <= 0 || divisions <= 0) {
        pn_set_error(error, cap, L"VPS has invalid tempo, meter, or divisions.");
        goto cleanup;
    }
    for (size_t index = 0; index < tempos.count; ++index) {
        tempos.items[index].beat = vps_bar_start(
            tempos.items[index].bar, pickup, song->meter_num,
            song->meter_den, &overrides
        );
    }
    if (tempos.count > 1) qsort(tempos.items, tempos.count, sizeof(VpsTempo), vps_tempo_compare);

    if (notes.count > UINT32_MAX) {
        pn_set_error(error, cap, L"VPS contains too many notes.");
        goto cleanup;
    }
    uint32_t order = 0;
    for (size_t index = 0; index < notes.count; ++index) {
        VpsNote note = notes.items[index];
        double start = vps_beat_seconds(note.start, song->bpm, &tempos);
        double end = vps_beat_seconds(note.start + note.duration, song->bpm, &tempos);
        uint64_t tick = (uint64_t)llround(note.start * 1000000.0);
        uint32_t note_id = (uint32_t)(index + 1);
        if (!pn_song_add_event(song, (PnEvent){
                start, tick, order++, note.bar, PN_NOTE_ON, 0,
                (uint8_t)note.midi, 80, note_id
            }, error, cap) ||
            !pn_song_add_event(song, (PnEvent){
                end, (uint64_t)llround((note.start + note.duration) * 1000000.0),
                order++, note.bar, PN_NOTE_OFF, 0, (uint8_t)note.midi, 0,
                note_id
            }, error, cap)) goto cleanup;
    }
    if (song->event_count == 0) {
        pn_set_error(error, cap, L"VPS contains no notes: %ls", path);
        goto cleanup;
    }
    pn_song_sort(song);
    song->duration = song->events[song->event_count - 1].time;
    success = true;

cleanup:
    for (size_t index = 0; index < voices.count; ++index) free(voices.items[index].previous_token);
    free(voices.items);
    free(notes.items);
    free(tempos.items);
    free(overrides.items);
    free(file_text);
    if (!success) pn_song_free(song);
    return success;
}
