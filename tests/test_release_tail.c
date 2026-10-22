#include "common.c"
#include <assert.h>

static void add(PnSong *song,double on,double off,int pitch,int channel,uint32_t id) {
    wchar_t error[PN_ERROR_CAP]=L"";
    PnEvent e={.time=on,.order=id*2,.type=PN_NOTE_ON,.channel=(uint8_t)channel,
               .data1=(uint8_t)pitch,.data2=80,.note_id=id};
    assert(pn_song_add_event(song,e,error,PN_ERROR_CAP));
    e.time=off;e.type=PN_NOTE_OFF;e.data2=0;e.order++;
    assert(pn_song_add_event(song,e,error,PN_ERROR_CAP));
}
static const PnEvent *event(const PnSong *song,uint32_t id,int type) {
    for(size_t i=0;i<song->event_count;++i)
        if(song->events[i].note_id==id&&song->events[i].type==type)return &song->events[i];
    assert(!"missing event");return NULL;
}
static void normalize(PnSong *song) {
    wchar_t error[PN_ERROR_CAP]=L"";pn_song_sort(song);
    assert(pn_normalize_note_events(song,true,error,PN_ERROR_CAP));
}
typedef struct CaseNote {double on,off;int pitch;uint32_t id;} CaseNote;
static int compare(const void *a,const void *b) {
    const CaseNote *x=a,*y=b;
    if(x->pitch!=y->pitch)return x->pitch<y->pitch?-1:1;
    return x->on<y->on?-1:x->on>y->on;
}
int main(void) {
    PnSong song;pn_song_init(&song);
    add(&song,0,1,60,0,1);add(&song,.4,.45,60,1,2);add(&song,.7,.75,60,0,3);
    normalize(&song);
    assert(song.event_count==6);
    assert(fabs(event(&song,1,PN_NOTE_OFF)->time-.398)<1e-9);
    assert(fabs(event(&song,2,PN_NOTE_OFF)->time-.698)<1e-9);
    assert(fabs(event(&song,3,PN_NOTE_OFF)->time-1.0)<1e-9);
    pn_song_free(&song);

    pn_song_init(&song);add(&song,0,.1,60,0,1);add(&song,.4,.45,60,0,2);
    normalize(&song);
    assert(fabs(event(&song,1,PN_NOTE_OFF)->time-.1)<1e-9);
    assert(fabs(event(&song,2,PN_NOTE_OFF)->time-.45)<1e-9);
    pn_song_free(&song);

    pn_song_init(&song);add(&song,0,.2,60,0,1);add(&song,.001,1,60,1,2);add(&song,.4,.5,60,0,3);
    normalize(&song);assert(song.event_count==4);
    assert(event(&song,1,PN_NOTE_OFF)->note_id==1);
    assert(fabs(event(&song,3,PN_NOTE_OFF)->time-1.0)<1e-9);
    pn_song_free(&song);

    for(int backend=0;backend<2;++backend) {
        pn_song_init(&song);add(&song,0,1,24,0,1);add(&song,.4,.45,36,1,2);
        PnOptions options={0};options.speed=1;options.fold=true;options.from_bar=1;
        options.start_delay=PN_DEFAULT_START_DELAY;options.keep_leading_silence=true;options.backend_midi=backend!=0;
        wchar_t error[PN_ERROR_CAP]=L"";pn_song_sort(&song);
        assert(pn_apply_options(&song,&options,error,PN_ERROR_CAP));
        assert(fabs(event(&song,1,PN_NOTE_ON)->time-PN_DEFAULT_START_DELAY)<1e-9);
        assert(event(&song,1,PN_NOTE_ON)->data1==36&&event(&song,2,PN_NOTE_ON)->data1==36);
        assert(fabs(event(&song,2,PN_NOTE_OFF)->time-(PN_DEFAULT_START_DELAY+1.0))<1e-9);
        pn_song_free(&song);
    }
    puts("PASS: overlapping long/short voices, repeated chains, real rests/staccato, coalesced note IDs, octave-fold collisions, both backends and 1.2s lead");

    enum {N=4000};CaseNote *original=calloc(N,sizeof(*original));assert(original);
    pn_song_init(&song);uint32_t state=42;
    for(uint32_t i=0;i<N;++i) {
        state=state*1664525u+1013904223u;
        double on=i*.005,off=on+.005+(state%2000)/1000.0;
        int pitch=36+(int)((state>>17)%61);
        original[i]=(CaseNote){on,off,pitch,i+1};add(&song,on,off,pitch,i%2,i+1);
    }
    normalize(&song);assert(song.event_count==N*2);
    qsort(original,N,sizeof(*original),compare);
    double latest_release[128]={0};
    for(size_t i=0;i<song.event_count;++i) {
        PnEvent e=song.events[i];
        if(e.type==PN_NOTE_ON) {
            assert(e.time>=latest_release[e.data1]-1e-9);
            latest_release[e.data1]=event(&song,e.note_id,PN_NOTE_OFF)->time;
        }
    }
    size_t clusters=0;
    for(size_t begin=0;begin<N;) {
        size_t end=begin+1;double expected=original[begin].off;
        while(end<N&&original[end].pitch==original[begin].pitch&&original[end].on<expected) {
            expected=fmax(expected,original[end].off);++end;
        }
        double actual=0;
        for(size_t i=begin;i<end;++i) {
            assert(fabs(event(&song,original[i].id,PN_NOTE_ON)->time-original[i].on)<1e-9);
            actual=fmax(actual,event(&song,original[i].id,PN_NOTE_OFF)->time);
        }
        assert(actual<=expected+1e-9&&expected-actual<=.002+1e-9);
        ++clusters;begin=end;
    }
    printf("PASS: %d generated notes, %zu source pitch-envelope clusters preserved; no added attacks, rests filled, or tails beyond source\n",N,clusters);
    free(original);pn_song_free(&song);return 0;
}
