#include "humanize.c"
#include <assert.h>

static void add(PnSong *song,double t,double length,int pitch,uint32_t id) {
    wchar_t error[PN_ERROR_CAP]=L"";
    PnEvent e={.time=t,.order=id*2,.note_id=id,.type=PN_NOTE_ON,.data1=(uint8_t)pitch,.data2=80};
    assert(pn_song_add_event(song,e,error,PN_ERROR_CAP));
    e.time+=length;e.type=PN_NOTE_OFF;e.data2=0;e.order++;
    assert(pn_song_add_event(song,e,error,PN_ERROR_CAP));
}
static void chord_releases(void) {
    for(int variant=0;variant<9;++variant) {
        PnSong song;pn_song_init(&song);HumanNote notes[4]={0};
        HumanGroup group={.begin=0,.end=3,.melody=SIZE_MAX};wchar_t error[PN_ERROR_CAP]=L"";
        double old[3];
        for(int i=0;i<3;++i) {
            double t=variant==4?i*.002:0;
            double length=variant==2?.04:variant==3?.46+i*.04:.5;
            int pitch=variant==8?36+i*25:60+i*4;
            add(&song,t,length,pitch,(uint32_t)i+1);
            if(variant==7)song.events[i*2].channel=song.events[i*2+1].channel=(uint8_t)i;
            notes[i]=(HumanNote){.on=(size_t)i*2,.off=(size_t)i*2+1,.time=t,.end=t+length,.id=(uint32_t)i+1,
                .pitch=(uint8_t)pitch,.channel=(uint8_t)(variant==7?i:0),.voice=SIZE_MAX,.previous=SIZE_MAX,.next=SIZE_MAX};
            old[i]=t+length;
            if(variant==5)notes[i].next=3;
        }
        notes[3].time=.05;
        if(variant==6) {
            PnEvent cc={.time=.498,.type=PN_CONTROL_CHANGE,.data1=64,.data2=127};
            assert(pn_song_add_event(&song,cc,error,PN_ERROR_CAP));
        }
        assert(human_neighbor_releases(&song,notes,&group,1,variant==1?.55:1,42,error,PN_ERROR_CAP));
        for(int i=0;i<3;++i) {
            assert(song.events[i*2].time==notes[i].time);
            assert(old[i]-song.events[i*2+1].time>=0&&old[i]-song.events[i*2+1].time<=.012+1e-9);
            if(variant>=2&&variant!=6)assert(song.events[i*2+1].time==old[i]);
            if(variant==6)assert(song.events[i*2+1].time>=.499-1e-9);
        }
        assert(song.events[1].time==old[0]);
        if(variant<=1)assert(song.events[3].time<.5&&song.events[5].time<.5&&song.events[3].time!=song.events[5].time);
        pn_song_free(&song);
    }
    puts("PASS: wide-chord release differentiation, preserved latest endpoint, source scaling, unequal/short/fast/separate/channel/distance exclusions, pedal boundary");
}
static void smooth_phrase(void) {
    HumanNote notes[33]={0};HumanGroup groups[33]={0};double residual[33];
    for(size_t i=0;i<33;++i) {
        notes[i].time=i*.5;notes[i].end=notes[i].time+.49;notes[i].id=(uint32_t)i+1;
        groups[i].begin=i;groups[i].end=i+1;groups[i].time=notes[i].time;
    }
    human_phrase_map(notes,groups,33,16.49,.5,1,42);
    double maximum=0;
    for(size_t i=0;i<33;++i) {
        double x=(double)i/32;
        double rubato=.030*(x*x-.6*sin(HUMAN_PI*x));
        residual[i]=groups[i].mapped-groups[i].time-rubato;
        assert(fabs(residual[i])<=.002+1e-9);
        if(i>=2)maximum=fmax(maximum,fabs(residual[i]-2*residual[i-1]+residual[i-2]));
        if(i)assert(groups[i].mapped>groups[i-1].mapped);
    }
    assert(fabs(residual[0])<1e-9&&fabs(residual[32])<1e-9&&maximum<.0002);
    puts("PASS: correlated sub-2ms phrase variation, smooth curvature, monotonic mapping and pinned jitter endpoints");
}
static void pedal_connection(void) {
    for(int variant=0;variant<8;++variant) {
        PnSong song;pn_song_init(&song);song.performance_source=variant==4;
        HumanNote notes[9]={0};HumanGroup groups[3]={0};wchar_t error[PN_ERROR_CAP]=L"";
        for(size_t g=0;g<3;++g) {
            groups[g]=(HumanGroup){.begin=g*3,.end=g*3+3,.time=g*.5,.mapped=g*.5,.confidence=1};
            for(size_t n=0;n<3;++n) {
                size_t i=g*3+n;int pitch=48+(int)g*12+(n==0?0:n==1?4:7);
                if(variant==1&&g==1)++pitch;
                double length=variant==2?.43:variant==3&&g==1?.10:.56;
                add(&song,g*.5,length,pitch,(uint32_t)i+1);
                notes[i]=(HumanNote){.on=i*2,.off=i*2+1,.time=g*.5,.end=g*.5+length,.pitch=(uint8_t)pitch};
            }
        }
        PnOptions opt={0};opt.backend_midi=variant!=5;opt.humanize=variant==7?1:2;
        assert(human_add_pedal(&song,notes,groups,3,&opt,.5,variant==6,error,PN_ERROR_CAP));
        size_t cc=0;double first_up=-1;
        for(size_t i=0;i<song.event_count;++i)if(song.events[i].type==PN_CONTROL_CHANGE) {
            ++cc;if(song.events[i].data2==0&&first_up<0)first_up=song.events[i].time;
        }
        if(variant==0)assert(cc==4&&fabs(first_up-.508)<1e-9);
        else if(variant<4)assert(first_up<.5);
        else assert(cc==0);
        pn_song_free(&song);
    }
    puts("PASS: 8ms same-harmony repedal only on written overlaps; no crossing new harmony/rest/staccato or invented source/Roblox/subtle pedal");
}
int main(void){chord_releases();smooth_phrase();pedal_connection();return 0;}
