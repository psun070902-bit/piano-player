#include "piano_native.h"
#include <assert.h>
#include <math.h>
#include <string.h>

/* Simulated clock/window state: these tests send no real keyboard input and
   never activate or close any user window. */
static LONGLONG fake_ms, regain_ms, second_loss, second_regain, stop_ms, close_ms;
static bool test_f8;
static int pauses, resumes, key_ups, key_downs;
static int midi_offs, midi_resets;
static int midi_ons;
static int partial_input = -1;
static bool capture_keys, capture_shift, scan_held[256], scan_released[256];
static LONGLONG scan_started[256], scan_ended[256];
static int pitch_for_scan[2][256], captured_pitches[128];
static bool midi_controllers[128];
static double progress;
static HWND fake_target = (HWND)(uintptr_t)1;
static BOOL fake_counter(LARGE_INTEGER *out) { out->QuadPart = fake_ms++; return TRUE; }
static HWND fake_foreground(void) {
    bool away = fake_ms < regain_ms || (second_loss && fake_ms >= second_loss && fake_ms < second_regain);
    return away ? (HWND)(uintptr_t)2 : fake_target;
}
static BOOL fake_is_window(HWND window) { return window == fake_target && (!close_ms || fake_ms < close_ms); }
static SHORT fake_key_state(int key) {
    if (key == VK_F8) return test_f8 &&
        ((fake_ms >= 110 && fake_ms < 190) || (fake_ms >= 250 && fake_ms < 310)) ? (SHORT)0x8000 : 0;
    return stop_ms && fake_ms >= stop_ms ? (SHORT)0x8000 : 0;
}
static void fake_sleep(DWORD ms) { fake_ms += ms; }
static DWORD fake_wait(HANDLE handle, DWORD ms) { (void)handle; fake_ms += ms; return WAIT_TIMEOUT; }
static UINT fake_send_input(UINT count, LPINPUT inputs, int size) {
    (void)size;
    UINT sent=count;
    if(partial_input>=0){if((UINT)partial_input<sent)sent=(UINT)partial_input;partial_input=-1;}
    for (UINT i = 0; i < sent; ++i) {
        if (inputs[i].ki.dwFlags & KEYEVENTF_KEYUP) ++key_ups;
        else { ++key_downs; assert(fake_foreground() == fake_target); }
        if(capture_keys) {
            unsigned scan=inputs[i].ki.wScan;
            assert(scan<256);
            bool up=(inputs[i].ki.dwFlags&KEYEVENTF_KEYUP)!=0;
            if(scan==MapVirtualKeyW(VK_LSHIFT,MAPVK_VK_TO_VSC))capture_shift=!up;
            else if(up) {
                assert(scan_held[scan]&&fake_ms-scan_started[scan]>=8);
                scan_held[scan]=false;scan_released[scan]=true;scan_ended[scan]=fake_ms;
            } else {
                assert(!scan_held[scan]);
                if(scan_released[scan])assert(fake_ms-scan_ended[scan]>=4);
                int pitch=pitch_for_scan[capture_shift?1:0][scan];assert(pitch>=36&&pitch<=96);
                ++captured_pitches[pitch];scan_held[scan]=true;scan_started[scan]=fake_ms;
            }
        }
    }
    return sent;
}
#define QueryPerformanceCounter fake_counter
#define GetForegroundWindow fake_foreground
#define IsWindow fake_is_window
#define GetAsyncKeyState fake_key_state
#define Sleep fake_sleep
#define WaitForSingleObject fake_wait
#define SendInput fake_send_input
static MMRESULT fake_midi_short(HMIDIOUT output, DWORD message) {
    assert(output == (HMIDIOUT)(uintptr_t)1);
    if ((message & 0xf0u) == 0x80u) ++midi_offs;
    if ((message & 0xf0u) == 0x90u) ++midi_ons;
    if ((message & 0xf0u) == 0xb0u) {
        assert(((message >> 16) & 127u) == 0);
        midi_controllers[(message >> 8) & 127u] = true;
    }
    return MMSYSERR_NOERROR;
}
static MMRESULT fake_midi_reset(HMIDIOUT output) {
    assert(output == (HMIDIOUT)(uintptr_t)1); ++midi_resets; return MMSYSERR_NOERROR;
}
#define midiOutShortMsg fake_midi_short
#define midiOutReset fake_midi_reset
#include "win_player.c"

static void pause_callback(void *unused, bool paused) {
    (void)unused; if (paused) ++pauses; else ++resumes;
}
static void progress_callback(void *unused, double elapsed, double total) {
    (void)unused; (void)total; progress = elapsed;
}
static void reset(void) {
    fake_ms = 100; regain_ms = 300; second_loss = second_regain = stop_ms = close_ms = 0;
    pauses = resumes = key_ups = key_downs = 0; progress = 0;
    test_f8 = false;
}

static void manual_pause_cases(void) {
    PnOptions options={0};options.quiet=true;options.pause_callback=pause_callback;
    wchar_t error[PN_ERROR_CAP]=L"";
    for(int variant=0;variant<4;++variant) {
        reset();test_f8=true;regain_ms=variant==2?300:0;
        if(variant==1)stop_ms=170;
        bool held[256]={false};uint32_t generation[256]={0};
        held['A']=true;generation['A']=1;
        PnClock clock={0};clock.frequency.QuadPart=1000;clock.held=held;clock.held_generation=generation;
        double last=-1;
        bool ok=pn_wait_until(&clock,variant==3?.30:.15,variant==3?NULL:fake_target,1,&options,&last,error,PN_ERROR_CAP);
        if(variant==0) {
            assert(ok&&pauses==1&&resumes==1&&!held['A']&&!generation['A']);
            assert(key_downs==0&&key_ups==2&&fake_ms>=275&&!clock.manual_paused);
            assert(fabs(pn_clock_now(&clock)-.15)<.010);
        } else if(variant==1) {
            assert(!ok&&pauses==1&&resumes==0&&key_downs==0&&fake_ms<200);
        } else if(variant==2) {
            assert(ok&&pauses==1&&resumes==1&&!clock.manual_paused); /* F8 in other app ignored */
        } else assert(ok&&pauses==0&&resumes==0&&held['A']&&!clock.manual_paused); /* MIDI independent */
    }
    reset();puts("PASS: F8 edge-triggered pause/resume, held-key cleanup, no repeated toggles/attacks, stop while paused, other-app and MIDI independence");
}

int main(void) {
    manual_pause_cases();
    PnOptions options = {0};
    options.quiet = true; options.pause_callback = pause_callback;
    options.progress_callback = progress_callback;
    wchar_t error[PN_ERROR_CAP] = L"";
    bool held[256] = {false}; uint32_t generations[256] = {0};
    PnClock clock = {0}; clock.frequency.QuadPart = 1000;
    clock.held = held; clock.held_generation = generations;
    double last_progress = -1;
    reset(); held['A'] = true; generations['A'] = 7;
    assert(pn_wait_until(&clock, .150, fake_target, 1.0, &options, &last_progress, error, PN_ERROR_CAP));
    assert(pauses == 1 && resumes == 1 && !held['A'] && generations['A'] == 0);
    assert(key_ups == 2 && key_downs == 0); /* A and stale Shift cleanup, no re-strike. */
    assert(fabs(pn_clock_now(&clock) - .150) < .008 && fake_ms >= 345);
    PnKeyEvent event = {.down = true, .vk = 'A', .scan = 30, .generation = 8};
    assert(pn_key_transition(&event, held, generations, error, PN_ERROR_CAP));
    assert(key_downs == 1);

    /* Two focus changes exclude both pauses rather than skipping/catching up. */
    reset(); second_loss = 400; second_regain = 650;
    memset(held, 0, sizeof(held)); clock.started.QuadPart = 0; last_progress = -1;
    assert(pn_wait_until(&clock, .350, fake_target, 1.0, &options, &last_progress, error, PN_ERROR_CAP));
    assert(pauses == 2 && resumes == 2 && key_downs == 0 && key_ups == 0);
    assert(fabs(pn_clock_now(&clock) - .350) < .010 && fake_ms >= 780);

    reset(); stop_ms = 170; clock.started.QuadPart = 0; last_progress = -1;
    assert(!pn_wait_until(&clock, .150, fake_target, 1.0, &options, &last_progress, error, PN_ERROR_CAP));
    assert(pauses == 1 && resumes == 0 && fake_ms < 200 && key_downs == 0);
    assert(pn_clock_now(&clock) < .120); /* stopped progress still excludes pause */

    reset(); close_ms = 180; clock.started.QuadPart = 0; last_progress = -1;
    assert(!pn_wait_until(&clock, .150, fake_target, 1.0, &options, &last_progress, error, PN_ERROR_CAP));
    assert(pauses == 1 && resumes == 0 && fake_ms < 210 && key_downs == 0);

    /* MIDI has no target-window guard: focus loss must not pause its clock. */
    reset(); clock.started.QuadPart = 0; last_progress = -1;
    assert(pn_wait_until(&clock, .150, NULL, 1.0, &options, &last_progress, error, PN_ERROR_CAP));
    assert(pauses == 0 && resumes == 0 && fake_ms < 165);
    /* Different MIDI lengths must survive the Roblox schedule, while repeated
       physical keys are released before the next press. Original mode stays short. */
    PnEvent sources[] = {
        {.time=.1, .type=PN_NOTE_ON, .data1=60, .data2=80, .note_id=1},
        {.time=.9, .type=PN_NOTE_OFF, .data1=60, .note_id=1},
        {.time=.2, .type=PN_NOTE_ON, .data1=64, .data2=80, .note_id=2},
        {.time=.25, .type=PN_NOTE_OFF, .data1=64, .note_id=2},
        {.time=.3, .type=PN_NOTE_ON, .data1=67, .data2=80, .note_id=3},
        {.time=.7, .type=PN_NOTE_OFF, .data1=67, .note_id=3},
        {.time=.6, .type=PN_NOTE_ON, .data1=60, .data2=80, .note_id=4},
        {.time=.75, .type=PN_NOTE_OFF, .data1=60, .note_id=4}
    };
    PnSong song = {.events=sources, .event_count=8};
    options.hold_seconds = .020;
    for (int mode = 0; mode < 2; ++mode) {
        options.humanize = mode ? 2 : 0;
        PnKeyEvent *keys = NULL; size_t key_count = 0;
        assert(pn_build_key_schedule(&song, &options, &keys, &key_count, error, PN_ERROR_CAP));
        assert(key_count == 8);
        size_t down_count = 0;
        for (size_t i = 0; i < key_count; ++i) {
            if (!keys[i].down) continue;
            ++down_count;
            size_t j = 0;
            while (j < key_count && (keys[j].down || keys[j].vk != keys[i].vk || keys[j].generation != keys[i].generation)) ++j;
            assert(j < key_count);
            double hold = keys[j].time - keys[i].time;
            if (!mode) assert(fabs(hold - .020) < 1e-9);
            else if (fabs(keys[i].time - .1) < 1e-9) assert(fabs(hold - .496) < 1e-9);
            else if (fabs(keys[i].time - .2) < 1e-9) assert(fabs(hold - .050) < 1e-9);
            else if (fabs(keys[i].time - .3) < 1e-9) assert(fabs(hold - .400) < 1e-9);
            else assert(fabs(hold - .150) < 1e-9);
        }
        assert(down_count == 4); free(keys);
    }
    /* Backend guard: an obsolete release must not kill the new strike. */
    uint32_t midi_active[128] = {0};
    PnEvent midi_event = {.type=PN_NOTE_ON,.data1=60,.data2=80,.note_id=10};
    assert(pn_midi_event((HMIDIOUT)(uintptr_t)1,&midi_event,midi_active,error,PN_ERROR_CAP));
    midi_event.note_id=11;
    assert(pn_midi_event((HMIDIOUT)(uintptr_t)1,&midi_event,midi_active,error,PN_ERROR_CAP));
    assert(midi_ons==2&&midi_offs==1&&midi_active[60]==11);
    midi_event.type=PN_NOTE_OFF;midi_event.note_id=10;
    assert(pn_midi_event((HMIDIOUT)(uintptr_t)1,&midi_event,midi_active,error,PN_ERROR_CAP));
    assert(midi_offs==1&&midi_active[60]==11);
    midi_event.note_id=11;
    assert(pn_midi_event((HMIDIOUT)(uintptr_t)1,&midi_event,midi_active,error,PN_ERROR_CAP));
    assert(midi_offs==2&&midi_active[60]==0);
    assert(pn_midi_event((HMIDIOUT)(uintptr_t)1,&midi_event,midi_active,error,PN_ERROR_CAP));
    assert(midi_offs==2);
    midi_offs=0;

    /* All 61 pitches map; stale Roblox key-ups are generation guarded too. */
    for(int pitch=36;pitch<=96;++pitch) {
        wchar_t character;int slot;UINT vk,scan;bool shifted;
        assert(pn_note_character(pitch,&character,&slot));
        assert(pn_key_info(character,&vk,&scan,&shifted));assert(scan&&slot==pitch-36);
    }
    reset();regain_ms=0;memset(held,0,sizeof(held));memset(generations,0,sizeof(generations));
    event=(PnKeyEvent){.down=true,.vk='A',.scan=30,.generation=8};
    assert(pn_key_transition(&event,held,generations,error,PN_ERROR_CAP));
    event.generation=9;assert(pn_key_transition(&event,held,generations,error,PN_ERROR_CAP));
    int before_ups=key_ups;event.down=false;event.generation=8;
    assert(pn_key_transition(&event,held,generations,error,PN_ERROR_CAP));
    assert(held['A']&&generations['A']==9&&key_ups==before_ups);
    event.generation=9;assert(pn_key_transition(&event,held,generations,error,PN_ERROR_CAP));
    assert(!held['A']&&key_ups==before_ups+1);

    /* Scheduler/OS lag: even overdue events must retain actual down/up dwell
       time rather than replaying an entire key sequence in a catch-up burst. */
    reset();regain_ms=0;fake_ms=1000;
    memset(held,0,sizeof(held));memset(generations,0,sizeof(generations));
    memset(&clock,0,sizeof(clock));clock.frequency.QuadPart=1000;
    clock.held=held;clock.held_generation=generations;last_progress=-1;
    event=(PnKeyEvent){.time=.1,.down=true,.vk='A',.scan=30,.generation=1};
    assert(pn_dispatch_key_event(&clock,&event,fake_target,2,&options,&last_progress,error,PN_ERROR_CAP));
    LONGLONG actual_down=clock.key_changed['A'];
    event.time=.108;event.down=false;
    assert(pn_dispatch_key_event(&clock,&event,fake_target,2,&options,&last_progress,error,PN_ERROR_CAP));
    LONGLONG actual_up=clock.key_changed['A'];assert(actual_up-actual_down>=8);
    event.time=.112;event.down=true;event.generation=2;
    assert(pn_dispatch_key_event(&clock,&event,fake_target,2,&options,&last_progress,error,PN_ERROR_CAP));
    assert(clock.key_changed['A']-actual_up>=4&&clock.started.QuadPart>0);
    event.time=.120;event.down=false;
    assert(pn_dispatch_key_event(&clock,&event,fake_target,2,&options,&last_progress,error,PN_ERROR_CAP));
    assert(!held['A']&&key_downs==2&&key_ups==2);

    /* Partial Shift batch must leave the possibly pressed note tracked so
       normal playback-error cleanup can release both note and Shift. */
    event=(PnKeyEvent){.down=true,.vk='A',.scan=30,.shifted=true,.generation=3};
    partial_input=2;
    assert(!pn_key_transition(&event,held,generations,error,PN_ERROR_CAP));
    assert(held['A']&&generations['A']==3);
    int cleanup_before=key_ups;pn_release_all_keys(held);
    assert(!held['A']&&key_ups==cleanup_before+2);

    /* End-to-end physical scan/Shift capture for an all-61-note chord. Each
       pitch must be transmitted exactly once, with no shared-key collisions. */
    PnEvent chord[122]={0};
    for(int p=36;p<=96;++p) {
        wchar_t ch;int slot;UINT vk,scan;bool shifted;
        assert(pn_note_character(p,&ch,&slot)&&pn_key_info(ch,&vk,&scan,&shifted));
        pitch_for_scan[shifted?1:0][scan]=p;
        chord[(p-36)*2]=(PnEvent){.time=2.5,.type=PN_NOTE_ON,.data1=(uint8_t)p,.note_id=(uint32_t)p};
        chord[(p-36)*2+1]=(PnEvent){.time=3,.type=PN_NOTE_OFF,.data1=(uint8_t)p,.note_id=(uint32_t)p};
    }
    PnSong chord_song={.events=chord,.event_count=122,.duration=3};
    PnKeyEvent *chord_keys=NULL;size_t chord_count=0;options.humanize=2;
    assert(pn_build_key_schedule(&chord_song,&options,&chord_keys,&chord_count,error,PN_ERROR_CAP));
    reset();regain_ms=0;memset(held,0,sizeof(held));memset(generations,0,sizeof(generations));
    memset(&clock,0,sizeof(clock));clock.frequency.QuadPart=1000;
    clock.held=held;clock.held_generation=generations;last_progress=-1;capture_keys=true;
    for(size_t i=0;i<chord_count;++i)
        assert(pn_dispatch_key_event(&clock,&chord_keys[i],fake_target,3,&options,&last_progress,error,PN_ERROR_CAP));
    capture_keys=false;free(chord_keys);
    assert(!capture_shift);
    for(int p=36;p<=96;++p)assert(captured_pitches[p]==1);
    for(int k=0;k<256;++k)assert(!scan_held[k]&&!held[k]);

    pn_midi_panic(NULL); assert(midi_offs == 0 && midi_resets == 0);
    pn_midi_panic((HMIDIOUT)(uintptr_t)1);
    assert(midi_offs == 128 && midi_resets == 1);
    assert(midi_controllers[64] && midi_controllers[66] && midi_controllers[67] && midi_controllers[123]);
    puts("PASS: focus/stop/close, MIDI independence, differentiated lengths, all-61-pitch chord scan/Shift capture, stale releases, overdue-event dwell/gap, partial Shift-batch cleanup, original mode, pedal cleanup");
    return 0;
}
