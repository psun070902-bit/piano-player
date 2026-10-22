/* Schedule-only integration tests. No keyboard or MIDI input is emitted. */
#include "win_player.c"
#include <assert.h>

static size_t failures,cases,attacks,moved;
static double max_move,max_total;
static void check(bool ok,const wchar_t *path,const char *why) {
    if(!ok&&failures++<15)fwprintf(stderr,L"FAIL %ls: %hs\n",path,why);
}
static void inspect(const PnSong *song,const PnSong *baseline,const PnOptions *options,
                    const wchar_t *path) {
    PnKeyEvent *keys=NULL;size_t count=0;wchar_t error[PN_ERROR_CAP]=L"";
    if(!pn_build_key_schedule(song,options,&keys,&count,error,PN_ERROR_CAP)) {
        check(false,path,"schedule rejected");fwprintf(stderr,L"%ls\n",error);return;
    }
    uint32_t max_id=0;size_t n=0;
    for(size_t i=0;i<song->event_count;++i) {
        if(song->events[i].type==PN_NOTE_ON)++n;
        if(song->events[i].note_id>max_id)max_id=song->events[i].note_id;
    }
    PnKeyNote *source=calloc(n,sizeof(*source));assert(source);
    double *original=calloc((size_t)max_id+1,sizeof(*original));assert(original);
    for(size_t i=0;i<baseline->event_count;++i) {
        PnEvent e=baseline->events[i];
        if(e.type==PN_NOTE_ON&&e.note_id<=max_id)original[e.note_id]=e.time;
    }
    size_t p=0;
    for(size_t i=0;i<song->event_count;++i) {
        PnEvent e=song->events[i];if(e.type!=PN_NOTE_ON)continue;
        wchar_t character;int slot;UINT vk,scan;bool shifted;
        assert(pn_note_character(e.data1,&character,&slot));
        assert(pn_key_info(character,&vk,&scan,&shifted));
        source[p++]=(PnKeyNote){e.time,character,slot,vk,scan,shifted,e.note_id,0};
    }
    qsort(source,n,sizeof(*source),pn_key_note_compare);
    size_t head[256],*next=calloc(n,sizeof(*next));assert(next);
    for(int k=0;k<256;++k)head[k]=SIZE_MAX;
    for(size_t i=n;i-->0;){next[i]=head[source[i].vk];head[source[i].vk]=i;}
    bool held[256]={0};uint32_t gen[256]={0};
    double down[256]={0},up[256];
    for(int k=0;k<256;++k)up[k]=-1;
    check(count==n*2,path,"lost or added note edges");
    size_t ons=0;
    for(size_t i=0;i<count;++i) {
        PnKeyEvent e=keys[i];
        if(i)check(e.time>=keys[i-1].time,path,"unsorted key schedule");
        if(e.down) {
            size_t s=head[e.vk];check(s!=SIZE_MAX,path,"extra attack");if(s==SIZE_MAX)continue;
            head[e.vk]=next[s];
            check(e.character==source[s].character&&e.shifted==source[s].shifted,path,"pitch/order changed");
            double delta=e.time-source[s].time;
            check(delta>=-1e-9&&delta<=PN_KEY_MAX_DELAY+1e-9,path,"unbounded transport delay");
            if(delta>1e-9)++moved;
            if(delta>max_move)max_move=delta;
            double total=fabs(e.time-original[source[s].generation]);if(total>max_total)max_total=total;
            check(total<.075,path,"combined expression and key delay exceeds 75ms");
            check(!held[e.vk],path,"physical key retrigger before release");
            if(up[e.vk]>=0)check(e.time-up[e.vk]>=PN_KEY_MIN_GAP-1e-9,path,"key-up gap below 4ms");
            if(!ons)check(fabs(e.time-PN_DEFAULT_START_DELAY)<1e-9,path,"first attack not at 1.2s");
            held[e.vk]=true;gen[e.vk]=e.generation;down[e.vk]=e.time;++ons;
        } else {
            check(held[e.vk]&&gen[e.vk]==e.generation,path,"orphan/stale key release");
            bool cutoff=song->end_limit>0&&fabs(e.time-song->duration)<1e-9;
            check(e.time-down[e.vk]>=(cutoff?.001:PN_KEY_MIN_HOLD)-1e-9,path,"key hold below minimum");
            held[e.vk]=false;up[e.vk]=e.time;
        }
        if(song->end_limit>0)check(e.time<=song->duration+1e-9,path,"key event past explicit cutoff");
    }
    check(ons==n,path,"attack count changed");
    for(int k=0;k<256;++k)check(!held[k]&&head[k]==SIZE_MAX,path,"stuck or missing key");
    attacks+=ons;++cases;free(keys);free(source);free(original);free(next);
}
static void synthetic(void) {
    PnOptions options={0};options.humanize=2;options.hold_seconds=.020;
    PnEvent notes[]={
        {.time=PN_DEFAULT_START_DELAY,.type=PN_NOTE_ON,.data1=60,.note_id=1},
        {.time=PN_DEFAULT_START_DELAY,.type=PN_NOTE_ON,.data1=61,.note_id=2},
        {.time=PN_DEFAULT_START_DELAY+.001,.type=PN_NOTE_ON,.data1=64,.note_id=3},
        {.time=3,.type=PN_NOTE_OFF,.data1=60,.note_id=1},
        {.time=3,.type=PN_NOTE_OFF,.data1=61,.note_id=2},
        {.time=3,.type=PN_NOTE_OFF,.data1=64,.note_id=3}};
    PnSong song={.events=notes,.event_count=6,.duration=3};
    inspect(&song,&song,&options,L"same-VK chord and unaffected E4");
    PnKeyEvent *chord_keys=NULL;size_t chord_count=0;wchar_t chord_error[PN_ERROR_CAP]=L"";
    assert(pn_build_key_schedule(&song,&options,&chord_keys,&chord_count,chord_error,PN_ERROR_CAP));
    wchar_t e4;int e4_slot;assert(pn_note_character(64,&e4,&e4_slot));
    bool found_e4=false;
    for(size_t i=0;i<chord_count;++i)if(chord_keys[i].down&&chord_keys[i].character==e4) {
        assert(fabs(chord_keys[i].time-(PN_DEFAULT_START_DELAY+.001))<1e-9);found_e4=true;
    }
    assert(found_e4);free(chord_keys);
    /* A pathological transport queue must fail clearly, never drop notes or
       silently smear a burst over a large fraction of a second. */
    PnEvent dense[20]={0};
    for(int i=0;i<10;++i){dense[i]=(PnEvent){.time=2.5+i*.002,.type=PN_NOTE_ON,.data1=60,.note_id=(uint32_t)i+1};
        dense[i+10]=(PnEvent){.time=2.501+i*.002,.type=PN_NOTE_OFF,.data1=60,.note_id=(uint32_t)i+1};}
    song.events=dense;song.event_count=20;song.duration=3;
    PnKeyEvent *keys=NULL;size_t count=0;wchar_t error[PN_ERROR_CAP]=L"";
    assert(!pn_build_key_schedule(&song,&options,&keys,&count,error,PN_ERROR_CAP));
    assert(wcsstr(error,L"too dense")&&keys==NULL);
}
int wmain(int argc,wchar_t **argv) {
    assert(argc==2);synthetic();PnSongList list={0};wchar_t error[PN_ERROR_CAP]=L"";
    assert(pn_discover_songs(argv[1],&list,error,PN_ERROR_CAP));
    const double speeds[]={.75,.8,.9,1,1.1,1.25};
    for(size_t i=0;i<list.count;++i)for(size_t s=0;s<6;++s) {
        PnOptions options={0};options.speed=speeds[s];options.fold=true;
        options.start_delay=PN_DEFAULT_START_DELAY;options.from_bar=1;options.keep_leading_silence=true;
        options.hold_seconds=.020;options.human_seed=42;
        PnSong baseline;assert(pn_load_song(list.items[i].path,&baseline,error,PN_ERROR_CAP));
        assert(pn_apply_options(&baseline,&options,error,PN_ERROR_CAP));
        for(int level=0;level<4;++level)for(int seed=0;seed<2;++seed) {
            options.humanize=level;options.human_seed=seed?123:42;
            PnSong song;assert(pn_load_song(list.items[i].path,&song,error,PN_ERROR_CAP));
            assert(pn_apply_options(&song,&options,error,PN_ERROR_CAP));
            inspect(&song,&baseline,&options,list.items[i].path);pn_song_free(&song);
        }
        pn_song_free(&baseline);
    }
    printf("%zu songs; %zu schedule cases; %zu attacks; %zu locally moved; max key delay %.6fms; max combined delta %.6fms; %zu failures\n",list.count,cases,attacks,moved,max_move*1000,max_total*1000,failures);
    pn_song_list_free(&list);return failures?1:0;
}
