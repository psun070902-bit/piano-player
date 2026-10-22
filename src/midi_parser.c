#include "piano_native.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

typedef struct PnReader {
    const uint8_t *data;
    size_t size;
    size_t pos;
} PnReader;

typedef struct PnRawEvent {
    uint64_t tick;
    uint32_t order;
    uint8_t type;
    uint8_t channel;
    uint8_t data1;
    uint8_t data2;
    uint32_t note_id;
} PnRawEvent;

typedef struct PnPendingNote {
    bool active;
    uint32_t note_id;
} PnPendingNote;

typedef struct PnRawEvents {
    PnRawEvent *items;
    size_t count;
    size_t capacity;
} PnRawEvents;

typedef struct PnTempo {
    uint64_t tick;
    uint32_t order;
    uint32_t microseconds;
} PnTempo;

typedef struct PnTempos {
    PnTempo *items;
    size_t count;
    size_t capacity;
} PnTempos;

static bool pn_reader_take(PnReader *reader, size_t count,
                           const uint8_t **result) {
    if (count > reader->size - reader->pos) return false;
    *result = reader->data + reader->pos;
    reader->pos += count;
    return true;
}

static bool pn_reader_u8(PnReader *reader, uint8_t *value) {
    const uint8_t *bytes;
    if (!pn_reader_take(reader, 1, &bytes)) return false;
    *value = bytes[0];
    return true;
}

static bool pn_reader_be16(PnReader *reader, uint16_t *value) {
    const uint8_t *bytes;
    if (!pn_reader_take(reader, 2, &bytes)) return false;
    *value = (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
    return true;
}

static bool pn_reader_be32(PnReader *reader, uint32_t *value) {
    const uint8_t *bytes;
    if (!pn_reader_take(reader, 4, &bytes)) return false;
    *value = ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
             ((uint32_t)bytes[2] << 8) | bytes[3];
    return true;
}

static bool pn_reader_varlen(PnReader *reader, uint32_t *value) {
    uint32_t result = 0;
    for (int count = 0; count < 4; ++count) {
        uint8_t byte;
        if (!pn_reader_u8(reader, &byte)) return false;
        if (result > 0x0FFFFFFFu >> 7) return false;
        result = (result << 7) | (byte & 0x7Fu);
        if (!(byte & 0x80u)) {
            *value = result;
            return true;
        }
    }
    return false;
}

static bool pn_raw_add(PnRawEvents *events, PnRawEvent event,
                       wchar_t *error, size_t cap) {
    if (events->count == events->capacity) {
        size_t next = events->capacity ? events->capacity * 2 : 8192;
        if (next < events->capacity || next > SIZE_MAX / sizeof(PnRawEvent)) {
            pn_set_error(error, cap, L"MIDI event array is too large.");
            return false;
        }
        PnRawEvent *resized = (PnRawEvent *)realloc(
            events->items, next * sizeof(PnRawEvent)
        );
        if (!resized) {
            pn_set_error(error, cap, L"Not enough memory while parsing MIDI.");
            return false;
        }
        events->items = resized;
        events->capacity = next;
    }
    events->items[events->count++] = event;
    return true;
}

static bool pn_tempo_add(PnTempos *tempos, PnTempo tempo,
                         wchar_t *error, size_t cap) {
    if (tempos->count == tempos->capacity) {
        size_t next = tempos->capacity ? tempos->capacity * 2 : 128;
        if (next < tempos->capacity || next > SIZE_MAX / sizeof(PnTempo)) {
            pn_set_error(error, cap, L"MIDI tempo map is too large.");
            return false;
        }
        PnTempo *resized = (PnTempo *)realloc(
            tempos->items, next * sizeof(PnTempo)
        );
        if (!resized) {
            pn_set_error(error, cap, L"Not enough memory for the tempo map.");
            return false;
        }
        tempos->items = resized;
        tempos->capacity = next;
    }
    tempos->items[tempos->count++] = tempo;
    return true;
}

static bool pn_read_file(const wchar_t *path, uint8_t **data, size_t *size,
                         wchar_t *error, size_t cap) {
    FILE *file = _wfopen(path, L"rb");
    if (!file) {
        pn_set_error(error, cap, L"Cannot open MIDI file: %ls", path);
        return false;
    }
    if (_fseeki64(file, 0, SEEK_END) != 0) {
        fclose(file);
        pn_set_error(error, cap, L"Cannot seek MIDI file: %ls", path);
        return false;
    }
    __int64 length = _ftelli64(file);
    if (length < 0 || (uint64_t)length > SIZE_MAX) {
        fclose(file);
        pn_set_error(error, cap, L"MIDI file is too large: %ls", path);
        return false;
    }
    rewind(file);
    uint8_t *buffer = (uint8_t *)malloc((size_t)length + 1);
    if (!buffer) {
        fclose(file);
        pn_set_error(error, cap, L"Not enough memory to read MIDI.");
        return false;
    }
    size_t read = fread(buffer, 1, (size_t)length, file);
    fclose(file);
    if (read != (size_t)length) {
        free(buffer);
        pn_set_error(error, cap, L"MIDI file could not be read completely.");
        return false;
    }
    *data = buffer;
    *size = read;
    return true;
}

static bool pn_name_is_marker(const wchar_t *name) {
    return _wcsicmp(name, L"@MP3_PERFORMANCE") == 0;
}

static void pn_consider_track_name(PnSong *song, const uint8_t *bytes,
                                   size_t length) {
    wchar_t name[PN_TITLE_CAP];
    if (!pn_utf8_to_wide((const char *)bytes, length, name, PN_TITLE_CAP)) return;
    while (*name && iswspace(*name)) memmove(name, name + 1, wcslen(name) * sizeof(wchar_t));
    size_t used = wcslen(name);
    while (used && iswspace(name[used - 1])) name[--used] = L'\0';
    if (!*name) return;
    if (_wcsnicmp(name, L"@END_SECONDS=", 13) == 0) {
        wchar_t *end;
        double limit = wcstod(name + 13, &end);
        if (*end == L'\0' && limit > 0.0 && limit < 86400.0) song->end_limit = limit;
        return;
    }
    if (pn_name_is_marker(name)) {
        song->performance_source = true;
        return;
    }
    if (_wcsicmp(song->title, L"Untitled") == 0) {
        pn_copy_wide(song->title, PN_TITLE_CAP, name);
    }
}

static bool pn_parse_track(PnReader track, int track_index,
                           PnRawEvents *events, PnTempos *tempos,
                           PnSong *song, uint32_t *global_order,
                           uint32_t *global_note_id,
                           wchar_t *error, size_t cap) {
    uint64_t tick = 0;
    uint8_t running_status = 0;
    PnPendingNote pending[16][128] = {{{false, 0}}};
    while (track.pos < track.size) {
        uint32_t delta;
        if (!pn_reader_varlen(&track, &delta) || UINT64_MAX - tick < delta) {
            pn_set_error(error, cap, L"Invalid delta time in MIDI track %d.", track_index + 1);
            return false;
        }
        tick += delta;

        uint8_t first;
        if (!pn_reader_u8(&track, &first)) {
            pn_set_error(error, cap, L"Unexpected end of MIDI track %d.", track_index + 1);
            return false;
        }
        uint8_t status;
        bool first_is_data = first < 0x80;
        if (first_is_data) {
            if (running_status < 0x80 || running_status >= 0xF0) {
                pn_set_error(error, cap, L"Invalid running status in MIDI track %d.", track_index + 1);
                return false;
            }
            status = running_status;
        } else {
            status = first;
            if (status < 0xF0) running_status = status;
        }

        if (status == 0xFF) {
            uint8_t meta_type;
            uint32_t length;
            const uint8_t *bytes;
            if (!pn_reader_u8(&track, &meta_type) ||
                !pn_reader_varlen(&track, &length) ||
                !pn_reader_take(&track, length, &bytes)) {
                pn_set_error(error, cap, L"Invalid meta event in MIDI track %d.", track_index + 1);
                return false;
            }
            uint32_t order = (*global_order)++;
            if ((meta_type == 0x03 || meta_type == 0x04) && length > 0) {
                pn_consider_track_name(song, bytes, length);
            } else if (meta_type == 0x51 && length == 3) {
                uint32_t tempo = ((uint32_t)bytes[0] << 16) |
                                 ((uint32_t)bytes[1] << 8) | bytes[2];
                if (tempo == 0 || !pn_tempo_add(
                        tempos, (PnTempo){tick, order, tempo}, error, cap)) {
                    if (tempo == 0) pn_set_error(error, cap, L"MIDI tempo cannot be zero.");
                    return false;
                }
                if (tick == 0 && song->bpm == 120.0) {
                    song->bpm = 60000000.0 / tempo;
                }
            } else if (meta_type == 0x58 && length >= 2 && tick == 0) {
                song->meter_num = bytes[0] ? bytes[0] : 4;
                song->meter_den = 1 << (bytes[1] > 6 ? 2 : bytes[1]);
            } else if (meta_type == 0x2F) {
                return true;
            }
            continue;
        }

        if (status == 0xF0 || status == 0xF7) {
            uint32_t length;
            const uint8_t *ignored;
            if (!pn_reader_varlen(&track, &length) ||
                !pn_reader_take(&track, length, &ignored)) {
                pn_set_error(error, cap, L"Invalid SysEx event in MIDI track %d.", track_index + 1);
                return false;
            }
            ++(*global_order);
            continue;
        }

        if (status >= 0xF0) {
            int system_length = (status == 0xF1 || status == 0xF3) ? 1 :
                                (status == 0xF2 ? 2 : 0);
            const uint8_t *ignored;
            if (!pn_reader_take(&track, (size_t)system_length, &ignored)) {
                pn_set_error(error, cap, L"Invalid system event in MIDI track %d.", track_index + 1);
                return false;
            }
            ++(*global_order);
            continue;
        }

        uint8_t command = status & 0xF0u;
        uint8_t channel = status & 0x0Fu;
        int data_length = (command == 0xC0 || command == 0xD0) ? 1 : 2;
        uint8_t data1 = 0, data2 = 0;
        if (first_is_data) data1 = first;
        else if (!pn_reader_u8(&track, &data1)) {
            pn_set_error(error, cap, L"Missing MIDI data byte in track %d.", track_index + 1);
            return false;
        }
        if (data_length == 2 && !pn_reader_u8(&track, &data2)) {
            pn_set_error(error, cap, L"Missing second MIDI data byte in track %d.", track_index + 1);
            return false;
        }
        if (data1 >= 0x80 || data2 >= 0x80) {
            pn_set_error(error, cap, L"Invalid MIDI data byte in track %d.", track_index + 1);
            return false;
        }

        uint32_t order = (*global_order)++;
        PnRawEvent event = {tick, order, 0, channel, data1, data2, 0};
        bool keep = true;
        if (command == 0x90 && data2 > 0) {
            if (channel == 9) {
                keep = false;
            } else {
                PnPendingNote *slot = &pending[channel][data1];
                if (slot->active) {
                    PnRawEvent release = {
                        tick, order, PN_NOTE_OFF, channel, data1, 0, slot->note_id
                    };
                    if (!pn_raw_add(events, release, error, cap)) return false;
                    order = (*global_order)++;
                }
                if (*global_note_id == UINT32_MAX) {
                    pn_set_error(error, cap, L"MIDI contains too many notes.");
                    return false;
                }
                event.order = order;
                event.type = PN_NOTE_ON;
                event.note_id = ++(*global_note_id);
                slot->active = true;
                slot->note_id = event.note_id;
            }
        } else if (command == 0x80 || (command == 0x90 && data2 == 0)) {
            PnPendingNote *slot = &pending[channel][data1];
            if (channel == 9 || !slot->active) {
                keep = false;
            } else {
                event.type = PN_NOTE_OFF;
                event.note_id = slot->note_id;
                slot->active = false;
            }
        } else if (command == 0xB0) {
            if (channel == 9) keep = false;
            else event.type = PN_CONTROL_CHANGE;
        }
        else keep = false;
        if (keep && !pn_raw_add(events, event, error, cap)) return false;
    }

    for (int channel = 0; channel < 16; ++channel) {
        for (int note = 0; note < 128; ++note) {
            PnPendingNote *slot = &pending[channel][note];
            if (!slot->active) continue;
            uint64_t release_tick = tick == UINT64_MAX ? tick : tick + 1;
            PnRawEvent release = {
                release_tick, (*global_order)++, PN_NOTE_OFF,
                (uint8_t)channel, (uint8_t)note, 0, slot->note_id
            };
            if (!pn_raw_add(events, release, error, cap)) return false;
        }
    }
    return true;
}

static int pn_raw_compare(const void *left_ptr, const void *right_ptr) {
    const PnRawEvent *left = (const PnRawEvent *)left_ptr;
    const PnRawEvent *right = (const PnRawEvent *)right_ptr;
    if (left->tick < right->tick) return -1;
    if (left->tick > right->tick) return 1;
    if (left->type < right->type) return -1;
    if (left->type > right->type) return 1;
    if (left->order < right->order) return -1;
    if (left->order > right->order) return 1;
    return 0;
}

static int pn_tempo_compare(const void *left_ptr, const void *right_ptr) {
    const PnTempo *left = (const PnTempo *)left_ptr;
    const PnTempo *right = (const PnTempo *)right_ptr;
    if (left->tick < right->tick) return -1;
    if (left->tick > right->tick) return 1;
    if (left->order < right->order) return -1;
    if (left->order > right->order) return 1;
    return 0;
}

bool pn_load_midi(const wchar_t *path, PnSong *song, wchar_t *error, size_t cap) {
    pn_song_init(song);
    uint8_t *file_data = NULL;
    size_t file_size = 0;
    PnRawEvents raw = {0};
    PnTempos tempos = {0};
    bool success = false;

    if (!pn_read_file(path, &file_data, &file_size, error, cap)) goto cleanup;
    PnReader file = {file_data, file_size, 0};
    const uint8_t *chunk_id;
    uint32_t header_length;
    uint16_t format, track_count, division;
    if (!pn_reader_take(&file, 4, &chunk_id) || memcmp(chunk_id, "MThd", 4) != 0 ||
        !pn_reader_be32(&file, &header_length) || header_length < 6 ||
        !pn_reader_be16(&file, &format) || !pn_reader_be16(&file, &track_count) ||
        !pn_reader_be16(&file, &division)) {
        pn_set_error(error, cap, L"Invalid MIDI header: %ls", path);
        goto cleanup;
    }
    if ((division & 0x8000u) || division == 0) {
        pn_set_error(error, cap, L"SMPTE MIDI timing is not supported: %ls", path);
        goto cleanup;
    }
    if (format > 1 || track_count == 0) {
        pn_set_error(error, cap, L"Unsupported MIDI format %u.", (unsigned)format);
        goto cleanup;
    }
    if (header_length > 6) {
        const uint8_t *ignored;
        if (!pn_reader_take(&file, header_length - 6, &ignored)) {
            pn_set_error(error, cap, L"Truncated MIDI header.");
            goto cleanup;
        }
    }
    song->division = division;
    uint32_t global_order = 0;
    uint32_t global_note_id = 0;
    for (uint16_t track_index = 0; track_index < track_count; ++track_index) {
        uint32_t track_length;
        if (!pn_reader_take(&file, 4, &chunk_id) || memcmp(chunk_id, "MTrk", 4) != 0 ||
            !pn_reader_be32(&file, &track_length)) {
            pn_set_error(error, cap, L"Missing MIDI track %u.", (unsigned)track_index + 1);
            goto cleanup;
        }
        const uint8_t *track_bytes;
        if (!pn_reader_take(&file, track_length, &track_bytes)) {
            pn_set_error(error, cap, L"Truncated MIDI track %u.", (unsigned)track_index + 1);
            goto cleanup;
        }
        PnReader track = {track_bytes, track_length, 0};
        if (!pn_parse_track(track, track_index, &raw, &tempos, song,
                            &global_order, &global_note_id, error, cap)) goto cleanup;
    }

    if (raw.count == 0) {
        pn_set_error(error, cap, L"MIDI contains no note or controller events: %ls", path);
        goto cleanup;
    }
    qsort(raw.items, raw.count, sizeof(PnRawEvent), pn_raw_compare);
    if (tempos.count) qsort(tempos.items, tempos.count, sizeof(PnTempo), pn_tempo_compare);

    uint64_t tempo_tick = 0;
    double tempo_seconds = 0.0;
    uint32_t current_tempo = 500000;
    size_t tempo_index = 0;
    double measure_ticks = (double)division * song->meter_num * 4.0 / song->meter_den;
    if (measure_ticks <= 0.0) measure_ticks = (double)division * 4.0;

    for (size_t index = 0; index < raw.count; ++index) {
        PnRawEvent source = raw.items[index];
        while (tempo_index < tempos.count && tempos.items[tempo_index].tick <= source.tick) {
            PnTempo point = tempos.items[tempo_index++];
            tempo_seconds += (double)(point.tick - tempo_tick) * current_tempo /
                             ((double)division * 1000000.0);
            tempo_tick = point.tick;
            current_tempo = point.microseconds;
        }
        double seconds = tempo_seconds +
                         (double)(source.tick - tempo_tick) * current_tempo /
                         ((double)division * 1000000.0);
        int bar = (int)((double)source.tick / measure_ticks) + 1;
        PnEvent event = {
            seconds, source.tick, source.order, bar,
            source.type, source.channel, source.data1, source.data2,
            source.note_id
        };
        if (!pn_song_add_event(song, event, error, cap)) goto cleanup;
    }
    pn_song_sort(song);
    song->duration = song->event_count ? song->events[song->event_count - 1].time : 0.0;
    success = true;

cleanup:
    free(file_data);
    free(raw.items);
    free(tempos.items);
    if (!success) pn_song_free(song);
    return success;
}
