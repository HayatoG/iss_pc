/* libSceAudioOut on SDL3 audio streams. sceAudioOutOutput returns once per
 * buffer period on a steady clock, like PS4 hardware: FMOD's output thread
 * reads 256-frame slots out of its mixer's 512-frame ring, and returning in
 * bursts (whenever the device pulls a quantum) lets it overtake the mixer and
 * play stale halves - a click every 10.7 ms. The lowest SDL queue level of
 * each window is steered to two buffers (silence prefill on underrun, ±3%
 * cadence corrections), which also absorbs device clock drift. Without an audio device (headless runs, TLG_AUDIO=none)
 * ports only follow the clock. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include "controller_audio.h"
#include "controller_ds4_bluetooth.h"
#include "controller_main_mix.h"
#include "controller_gain.h"
#include "dualsense_bt_sink.h"
#include "iss_power_wheel.h"
#include <math.h>
int tlggpu_pad_speaker_enabled(void); /* gpu shim settings (Insert > Controls) */
/* Pad-speaker sounds played through the PC speakers (no USB DualSense, Bluetooth, unplugged): the game
 * mixes them loud (Neon absorbing especially), so they play at half level there (2026-11-06; was 0.75,
 * still too loud). ISS_PAD_SPEAKER_PC_GAIN, 0..2, default 0.5. */
static float controller_pc_level(void) {
    static float level=-1.f;
    if (level<0.f) {
        const char *e=getenv("ISS_PAD_SPEAKER_PC_GAIN");char *end=NULL;float v=e?strtof(e,&end):0.5f;
        level=(e && (end==e || *end)) || !(v>=0.f && v<=2.f) ? 0.5f : v;
    }
    return level;
}

#define PORTS 25
#define ERR_NOT_OPENED ((int32_t)0x80260001)
#define ERR_INVALID_PORT ((int32_t)0x80260003)
#define ERR_INVALID_POINTER ((int32_t)0x80260004)
#define ERR_PORT_FULL ((int32_t)0x80260005)
#define ERR_INVALID_SIZE ((int32_t)0x80260006)
#define ERR_INVALID_FORMAT ((int32_t)0x80260007)
#define ERR_INVALID_FREQ ((int32_t)0x80260008)
#define ERR_INVALID_VOLUME ((int32_t)0x80260009)
#define ERR_INVALID_TYPE ((int32_t)0x8026000A)
#define ERR_ALREADY_INIT ((int32_t)0x8026000E)
#define ERR_NOT_INIT ((int32_t)0x8026000F)
#define VOLUME_0DB 32768

typedef struct {
    int used, type, channels, is_float, frames, sample_bytes, std_layout, downmix;
    int32_t volume[8];
    SDL_AudioStream *stream;
    SDL_AudioDeviceID controller_device;
    Ds4BluetoothSpeaker *bluetooth_speaker;
    bool bluetooth_reserved;
    DsBtSink *bt;                   /* ISS_PAD_SPEAKER_BLUETOOTH=1: speaker reports instead of a stream */
    int controller_stereo;
    int controller_enabled,controller_signal_logged;
    float controller_gain;
    float pc_limit;            /* PC-speaker limiter gain for pad sounds (1 = no reduction) */
    float pc_loudest;          /* loudest pad sample seen so far (logged when it grows) */
    uint64_t controller_check_ns;
    uint64_t ds4_retry_ns;     /* last late DualShock 4 Bluetooth attach attempt */
    uint64_t next_deadline_ns; /* next return of sceAudioOutOutput */
    int64_t adjust_ns; int window_min, window_count; /* queue level control */
    uint64_t last_output_us;
    pthread_mutex_t lock;
    FILE *dump;                     /* TLG_AUDIO_DUMP: raw converted PCM per port */
    int stats;
    PcStereoLimiter main_mix;
    uint64_t stat_start_ns, stat_last_ns, stat_max_gap_ns; /* TLG_AUDIO_STATS */
    unsigned stat_starved, stat_buffers; int stat_min_queued;
} Port;
typedef struct { uint16_t output; uint8_t channel, reserved; int16_t volume; uint16_t reroute; uint64_t flag, reserved64[2]; } PortState;
_Static_assert(sizeof(PortState)==32,"AudioOut port state layout");

static pthread_mutex_t table_lock=PTHREAD_MUTEX_INITIALIZER;
static Port ports[PORTS];
static int initialized, sdl_ready=-1;
static size_t buffers_out, ports_opened;
static unsigned controller_ports; /* protected by table_lock */
static bool bluetooth_reserved; /* protected by table_lock */
static pthread_mutex_t bluetooth_route_lock=PTHREAD_MUTEX_INITIALIZER;
static Ds4BluetoothSpeaker *bluetooth_route;
static SDL_JoystickID bluetooth_route_id;

int runtime_audio_bluetooth_rumble(uint32_t id,uint16_t low,uint16_t high,uint32_t ms) {
    pthread_mutex_lock(&bluetooth_route_lock);
    Ds4BluetoothSpeaker *speaker=bluetooth_route;
    const bool handled=speaker && id==bluetooth_route_id;
    if (handled) {
        SDL_LockMutex(speaker->effects_lock);
        const Uint8 low_motor=ms?(Uint8)(low>>8):0;
        const Uint8 high_motor=ms?(Uint8)(high>>8):0;
        if (speaker->low_motor!=low_motor || speaker->high_motor!=high_motor)
            speaker->effects_dirty|=1;
        speaker->low_motor=low_motor;
        speaker->high_motor=high_motor;
        speaker->rumble_until=low_motor || high_motor?SDL_GetTicksNS()+(Uint64)ms*1000000:0;
        SDL_UnlockMutex(speaker->effects_lock);
    }
    pthread_mutex_unlock(&bluetooth_route_lock);
    return handled;
}

int runtime_audio_bluetooth_led(uint32_t id,const uint8_t rgb[3]) {
    pthread_mutex_lock(&bluetooth_route_lock);
    Ds4BluetoothSpeaker *speaker=bluetooth_route;
    const bool handled=speaker && id==bluetooth_route_id;
    if (handled) {
        SDL_LockMutex(speaker->effects_lock);
        if (SDL_memcmp(speaker->rgb,rgb,3)) speaker->effects_dirty|=2;
        SDL_memcpy(speaker->rgb,rgb,3);
        SDL_UnlockMutex(speaker->effects_lock);
    }
    pthread_mutex_unlock(&bluetooth_route_lock);
    return handled;
}

static void close_bluetooth_speaker(Ds4BluetoothSpeaker *speaker) {
    pthread_mutex_lock(&bluetooth_route_lock);
    if (bluetooth_route==speaker) {
        bluetooth_route=NULL;
        bluetooth_route_id=0;
    }
    pthread_mutex_unlock(&bluetooth_route_lock);
    ds4_bt_close(speaker);
}

static Ds4BluetoothSpeaker *open_bluetooth_speaker(const SDL_AudioSpec *spec) {
    if (bluetooth_reserved || !SDL_WasInit(SDL_INIT_GAMEPAD)) return NULL;
    int count=0;
    SDL_JoystickID *ids=SDL_GetGamepads(&count);
    SDL_Gamepad *pad=ids && count==1 ? SDL_OpenGamepad(ids[0]) : NULL;
    SDL_free(ids);
    if (!pad) return NULL;
    Ds4BluetoothSpeaker *speaker=ds4_bt_open(pad,spec);
    if (speaker) {
        pthread_mutex_lock(&bluetooth_route_lock);
        bluetooth_route=speaker;
        bluetooth_route_id=SDL_GetGamepadID(pad);
        pthread_mutex_unlock(&bluetooth_route_lock);
    }
    SDL_CloseGamepad(pad);
    return speaker;
}

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec; }
static void sleep_until(uint64_t deadline) {
#ifdef _WIN32
    /* winpthreads' clock_nanosleep only takes CLOCK_REALTIME (EINVAL otherwise). */
    for (uint64_t now=now_ns(); now<deadline; now=now_ns()) compat_sleep_ns(deadline-now);
#else
    struct timespec t={(time_t)(deadline/1000000000u),(long)(deadline%1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,NULL)) {}
#endif
}
static int sdl_audio(void) {
    if (sdl_ready<0) {
        const char *mode=getenv("TLG_AUDIO");
        sdl_ready = (!mode || strcmp(mode,"none")) && SDL_InitSubSystem(SDL_INIT_AUDIO);
        printf("Runtime: audio backend %s\n", sdl_ready ? SDL_GetCurrentAudioDriver() : "timer (silent)");
    }
    return sdl_ready;
}
static int port_range(int type, int *first, int *last) {
    switch (type) {
    case 0: *first=0; *last=7; return 1;      /* main */
    case 1: *first=8; *last=8; return 1;      /* bgm */
    case 2: *first=9; *last=12; return 1;     /* voice */
    case 3: *first=13; *last=16; return 1;    /* personal */
    case 4: *first=17; *last=20; return 1;    /* pad speaker */
    case 126: *first=21; *last=22; return 1;  /* audio3d */
    case 127: *first=23; *last=24; return 1;  /* aux */
    default: return 0;
    }
}
/* SDL's 7.1-to-stereo matrix keeps only ~0.21 of the front channels (-13.5 dB), so the game
   was far too quiet on stereo outputs. Folds SDL-ordered FL FR C LFE BL BR SL SR frames to
   stereo in place: fronts at full level, center and surrounds at -3 dB, LFE dropped, clamped.
   Returns the stereo byte count. */
static size_t downmix_stereo(unsigned char *buf, size_t frames, int is_float) {
    const float k=0.70710678f;
    for (size_t f=0;f<frames;++f) {
        float s[8], lr[2];
        for (int c=0;c<8;++c) {
            if (is_float) memcpy(&s[c],buf+(f*8+(size_t)c)*4,4);
            else { int16_t v; memcpy(&v,buf+(f*8+(size_t)c)*2,2); s[c]=(float)v/32768.0f; }
        }
        lr[0]=s[0]+k*(s[2]+s[4]+s[6]); lr[1]=s[1]+k*(s[2]+s[5]+s[7]);
        for (int c=0;c<2;++c) {
            float v=lr[c]<-1.0f ? -1.0f : lr[c]>1.0f ? 1.0f : lr[c];
            if (is_float) memcpy(buf+(f*2+(size_t)c)*4,&v,4);
            else { int16_t o=(int16_t)(v*32767.0f); memcpy(buf+(f*2+(size_t)c)*2,&o,2); }
        }
    }
    return frames*2*(is_float ? 4u : 2u);
}
static Port *port_of(int32_t handle, int32_t *error) {
    int id=handle & 0xff;
    if ((handle & 0x3f000000)!=0x20000000 || id>=PORTS) { *error=ERR_INVALID_PORT; return NULL; }
    if (!ports[id].used) { *error=ERR_NOT_OPENED; return NULL; }
    return &ports[id];
}

static ABI int32_t audio_init(void) {
    pthread_mutex_lock(&table_lock);
    int32_t r=initialized ? ERR_ALREADY_INIT : 0;
    initialized=1;
    pthread_mutex_unlock(&table_lock);
    if (runtime_verbose()) printf("Verbose: sceAudioOutInit -> %#x\n",(unsigned)r);
    return r;
}
static int32_t open_port(int32_t type, uint32_t length, uint32_t freq, uint32_t param) {
    if (runtime_verbose() && ((!length || length>2048 || (length & 0xff)) || freq!=48000 || (param & 0xff)>7))
        printf("Verbose: sceAudioOutOpen refused: type %d, length %u, freq %u, param %#x\n",type,length,freq,param);
    if (!length || length>2048 || (length & 0xff)) return ERR_INVALID_SIZE;
    if (freq!=48000) return ERR_INVALID_FREQ;
    uint32_t format=param & 0xff;
    if (format>7) return ERR_INVALID_FORMAT;
    int first, last;
    if (!port_range(type,&first,&last)) return ERR_INVALID_TYPE;
    static const int channels[8]={1,2,8,1,2,8,8,8};
    pthread_mutex_lock(&table_lock);
    int id=-1;
    for (int i=first;i<=last;++i) if (!ports[i].used) { id=i; break; }
    if (id<0) { pthread_mutex_unlock(&table_lock); return ERR_PORT_FULL; }
    Port *p=&ports[id];
    memset(p,0,sizeof(*p));
    pthread_mutex_init(&p->lock,NULL);
    p->used=1; p->type=type; p->channels=channels[format]; p->is_float=format>=3 && format!=6;
    p->sample_bytes=p->is_float ? 4 : 2; p->frames=(int)length; p->std_layout=format>=6;
    p->controller_gain=1.f;
    if (type==4) {
        float gain=controller_gain_live();
        p->controller_gain=gain;
        printf("Controller feedback: controller-speaker gain %.2fx (main audio unchanged)\n",gain);
    }
    for (int c=0;c<8;++c) p->volume[c]=VOLUME_0DB;
    if (sdl_audio()) {
        SDL_AudioDeviceID target=SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
        int dedicated=0,muted=0;
        if (type==4)target=controller_audio_device(getenv("ISS_PAD_SPEAKER_DEVICE"),&dedicated,&muted);

        if (type==4 && !dedicated && !muted) {
            const char *choice=getenv("ISS_PAD_SPEAKER_DEVICE");
            if (!choice || !*choice || !SDL_strcasecmp(choice,"auto")) {
                const SDL_AudioSpec spec={p->is_float?SDL_AUDIO_F32:SDL_AUDIO_S16,p->channels,48000};
                p->bluetooth_speaker=open_bluetooth_speaker(&spec);
                if (p->bluetooth_speaker) {
                    bluetooth_reserved=p->bluetooth_reserved=true;
                    puts("Controller feedback: pad-speaker audio routed directly to DualShock 4 over Bluetooth");
                }
            }
        }
        if (type==4 && !dedicated && !muted && !p->bluetooth_speaker && dualsense_bt_speaker_requested()) {
            /* No USB speaker endpoint and no explicit device: try a Bluetooth DualSense. */
            const char *choice=getenv("ISS_PAD_SPEAKER_DEVICE");
            if (!choice || !*choice || !SDL_strcasecmp(choice,"auto")) p->bt=ds_bt_sink_open();
        }
        SDL_AudioSpec device; int device_frames;
        p->downmix=p->channels==8 && SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&device,&device_frames)
                   && device.channels==2;
        p->controller_stereo=dedicated && p->channels==1;
        SDL_AudioSpec spec={p->is_float ? SDL_AUDIO_F32 : SDL_AUDIO_S16, p->downmix || p->controller_stereo ? 2 : p->channels, 48000};
        if (!muted && !p->bt && !p->bluetooth_speaker)p->stream=SDL_OpenAudioDeviceStream(target,&spec,NULL,NULL);
        if (dedicated && p->stream && !controller_audio_speaker_map(p->stream)) {
            fprintf(stderr,"Controller feedback: could not isolate speaker channels; using normal PC output\n");
            SDL_DestroyAudioStream(p->stream);p->stream=NULL;
        }
        if (!muted && dedicated && !p->stream) {
            dedicated=0;p->controller_stereo=0;spec.channels=p->downmix?2:p->channels;
            p->stream=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,NULL,NULL);
        }
        if (p->stream && !SDL_ResumeAudioStreamDevice(p->stream)) {
            fprintf(stderr,"Controller feedback: audio device failed to resume: %s\n",SDL_GetError());
            SDL_DestroyAudioStream(p->stream);p->stream=NULL;
            if (dedicated) {
                dedicated=0;p->controller_stereo=0;spec.channels=p->downmix?2:p->channels;
                p->stream=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,NULL,NULL);
                if (p->stream)SDL_ResumeAudioStreamDevice(p->stream);
            }
        }
        if (dedicated && p->stream) {
            p->controller_device=target;
            if (controller_audio_name(SDL_GetAudioDeviceName(target))) {
                p->controller_enabled=1;++controller_ports;runtime_pad_controller_speaker(1);
            }
            printf("Controller feedback: pad-speaker audio routed to %s; extra channels muted\n",SDL_GetAudioDeviceName(target));
        } else if (p->bluetooth_speaker) {
            p->controller_gain=1.f;
        } else if (p->bt) {
            p->controller_gain=1.f; /* unity: the speaker preamp is on; boosted gain clipped in other ports */
            p->controller_enabled=1;++controller_ports;runtime_pad_controller_speaker(1);
            puts("Controller feedback: pad-speaker audio routed to the Bluetooth DualSense speaker (prototype)");
        } else if (type==4) {
            p->controller_gain=controller_pc_level(); /* PC output: no controller boost, half the game's level */
            printf("Controller feedback: pad-speaker audio uses %s (level %.2fx)\n",muted?"muted timer sink":"normal PC output",p->controller_gain);
        }
        if (!p->stream && !muted && !p->bt && !p->bluetooth_speaker)fprintf(stderr,"Runtime: SDL audio stream failed (%s); port %d uses the timer sink\n",SDL_GetError(),id);
    }
    p->stats=getenv("TLG_AUDIO_STATS")!=NULL;
    const char *dump=getenv("TLG_AUDIO_DUMP");
    if (dump) {
        char path[4096];
        snprintf(path,sizeof(path),"%s.port%d.%dch.%s",dump,id,p->downmix ? 2 : p->channels,p->is_float ? "f32" : "s16");
        p->dump=fopen(path,"wb");
    }
    ++ports_opened;
    pthread_mutex_unlock(&table_lock);
    printf("Runtime: audio port %d opened (type %d, %d ch, %s, %u frames%s)\n",id,type,p->channels,p->is_float ? "float" : "s16",length,
           p->downmix ? ", downmixed to stereo" : "");
    if (runtime_verbose()) printf("Verbose: audio port %d: SDL stream %s, format code %u\n",id,p->stream ? "open" : "none (timer sink)",format);
    return (type<<16) | id | 0x20000000;
}
static ABI int32_t audio_open(int32_t user, int32_t type, int32_t index, uint32_t length, uint32_t freq, uint32_t param) {
    (void)user; (void)index;
    if (!initialized) return ERR_NOT_INIT;
    return open_port(type,length,freq,param);
}
static ABI int32_t audio_close(int32_t handle) {
    pthread_mutex_lock(&table_lock);
    int32_t error=0;
    Port *p=port_of(handle,&error);
    if (p) {
        pthread_mutex_lock(&p->lock);
        if (p->stream) SDL_DestroyAudioStream(p->stream);
        ds_bt_sink_close(p->bt);p->bt=NULL;
        if (p->bluetooth_speaker) close_bluetooth_speaker(p->bluetooth_speaker);
        p->bluetooth_speaker=NULL;
        if (p->bluetooth_reserved) bluetooth_reserved=false;

        if (p->controller_enabled && controller_ports && !--controller_ports)runtime_pad_controller_speaker(0);
        if (p->dump) fclose(p->dump);
        p->dump=NULL;
        p->stream=NULL; p->used=0;
        pthread_mutex_unlock(&p->lock);
    }
    pthread_mutex_unlock(&table_lock);
    return p ? 0 : error;
}
/* `pace`: wait for this port's next period. sceAudioOutOutputs waits once for all its ports. */
static int32_t output_port(int32_t handle, const void *data, int pace) {
    int32_t error=0;
    pthread_mutex_lock(&table_lock);
    Port *p=port_of(handle,&error);
    pthread_mutex_unlock(&table_lock);
    if (!p) return error;
    pthread_mutex_lock(&p->lock);
    if (p->bluetooth_speaker && SDL_GetAtomicInt(&p->bluetooth_speaker->failed)) {
        close_bluetooth_speaker(p->bluetooth_speaker);
        p->bluetooth_speaker=NULL;
        const SDL_AudioSpec spec={p->is_float?SDL_AUDIO_F32:SDL_AUDIO_S16,p->channels,48000};
        p->stream=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,NULL,NULL);
        if (p->stream) SDL_ResumeAudioStreamDevice(p->stream);
        puts("Controller feedback: DualShock 4 Bluetooth disconnected; using limited PC output");
    }
    /* Late DualShock 4 attach (2026-11-06): the game opens the pad-speaker port before SDL reports
     * the controller, so the open-time check found no pad. While pad sounds use the PC fallback,
     * look again once a second and move them to a wireless DS4 when one appears. table_lock is
     * only tried (close takes it before p->lock), so a busy table just waits for the next second. */
    if (p->type==4 && p->stream && !p->bluetooth_speaker && !p->bt && !p->controller_device &&
        now_ns()-p->ds4_retry_ns>=1000000000u) {
        p->ds4_retry_ns=now_ns();
        const char *choice=getenv("ISS_PAD_SPEAKER_DEVICE");
        if ((!choice || !*choice || !SDL_strcasecmp(choice,"auto")) && !pthread_mutex_trylock(&table_lock)) {
            const SDL_AudioSpec spec={p->is_float?SDL_AUDIO_F32:SDL_AUDIO_S16,p->channels,48000};
            Ds4BluetoothSpeaker *speaker=open_bluetooth_speaker(&spec);
            if (speaker) {
                bluetooth_reserved=p->bluetooth_reserved=true;
                SDL_DestroyAudioStream(p->stream);p->stream=NULL;
                p->pc_limit=1.f;p->pc_loudest=0.f;
                puts("Controller feedback: pad-speaker audio routed directly to DualShock 4 over Bluetooth (connected after start)");
                p->bluetooth_speaker=speaker;
            }
            pthread_mutex_unlock(&table_lock);
        }
    }

    if (p->type==4) {
        float gain=controller_gain_live();
        /* Neon 10% quieter on the controller speaker (2026-11-06): the pad stream is one mix, so
         * the whole controller output is lowered while Neon is the active power.
         * ISS_PAD_SPEAKER_NEON_GAIN (0..2, default 0.9); unknown power: unchanged. */
        static float neon=-1.f;
        if (neon<0.f) {
            const char *e=getenv("ISS_PAD_SPEAKER_NEON_GAIN");char *end=NULL;float v=e?strtof(e,&end):0.9f;
            neon=(e && (end==e || *end)) || !(v>=0.f && v<=2.f) ? 0.9f : v;
        }
        IssPowerWheelStatus power;runtime_power_wheel_status(&power);
        if (power.supported && power.ready && power.active==1 /* PW_NEON */) gain*=neon;
        /* The boost (and Neon level) is for the controller's own speaker only: when the pad sounds
         * play through the PC speakers (no USB DualSense, Bluetooth, or unplugged), they keep the
         * game's normal level instead of 4x (2026-11-06). */
        /* Bluetooth speaker: Smoke and Video were too quiet at unity, so they get ISS_PAD_SPEAKER_BT_BOOST
         * (default 2.0, 0..4); Neon/Concrete stay at unity. The limiter below keeps boosted peaks unclipped. */
        static float bt_boost=-1.f;
        if (bt_boost<0.f) {
            const char *e=getenv("ISS_PAD_SPEAKER_BT_BOOST");char *end=NULL;float v=e?strtof(e,&end):2.f;
            bt_boost=(e && (end==e || *end)) || !(v>=0.f && v<=4.f) ? 2.f : v;
        }
        const int smoke_or_video=power.supported && power.ready && (power.active==0 || power.active==2);
        if (!p->controller_device) gain=(p->bt || p->bluetooth_speaker) ? (smoke_or_video ? bt_boost : 1.f) : controller_pc_level();
        /* Insert > Controls > "Play controller-speaker sounds" off: mute these sounds on every route. */
        if (!tlggpu_pad_speaker_enabled()) gain=0.f;
        if (gain!=p->controller_gain) {p->controller_gain=gain;printf("Controller feedback: live speaker gain %.2fx%s\n",gain,
            power.active==1 && neon!=1.f ? " (Neon)" : "");}
    }
    if (p->controller_device && now_ns()-p->controller_check_ns>=1000000000u) {
        p->controller_check_ns=now_ns();
        if (!SDL_GetAudioDeviceName(p->controller_device)) {
            SDL_DestroyAudioStream(p->stream);p->stream=NULL;p->controller_device=0;p->controller_stereo=0;
            SDL_AudioSpec spec={p->is_float?SDL_AUDIO_F32:SDL_AUDIO_S16,p->channels,48000};
            p->stream=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,NULL,NULL);
            if (p->stream)SDL_ResumeAudioStreamDevice(p->stream);
            puts("Controller feedback: controller audio disconnected; pad sounds returned to normal PC output (restart to reroute)");
            /* Keep ownership until close; the absent controller cannot receive output. */
        }
    }
    size_t samples=(size_t)p->frames*(size_t)p->channels, bytes=samples*(size_t)p->sample_bytes;
    uint64_t period=(uint64_t)p->frames*1000000000u/48000u;
    if (data) {
        /* Apply per-channel volume and map PS4 8ch (L R C LFE SL SR BL BR) to SDL 7.1 order. */
        unsigned char converted[2048*8*4];
        static const int remap[8]={0,1,2,3,6,7,4,5};
        int nonzero=0;
        const int monitor=p->type==4 && !p->controller_signal_logged;
        float source_peak=0.f;
        for (size_t f=0;f<(size_t)p->frames;++f) for (int c=0;c<p->channels;++c) {
            int target=p->channels==8 && !p->std_layout ? remap[c] : c;
            size_t from=f*(size_t)p->channels+(size_t)c, to=f*(size_t)p->channels+(size_t)target;
            float gain=(float)p->volume[c]/VOLUME_0DB*p->controller_gain;
            if (p->is_float) {
                float v;memcpy(&v,(const char *)data+from*4,4);
                if(p->type==4) {float a=v<0?-v:v;if(a>source_peak)source_peak=a;}
                v*=gain;
                /* PC-speaker pad sounds are limited below instead of being cut off here. */
                if (p->type==4 && p->controller_device) {if(v>1.f)v=1.f;else if(v< -1.f)v=-1.f;}
                memcpy(converted+to*4,&v,4);if(monitor && (v>0.00001f || v< -0.00001f))nonzero=1;
            } else {
                int16_t v;memcpy(&v,(const char *)data+from*2,2);
                if(p->type==4) {float a=(v<0?-(float)v:(float)v)/32768.f;if(a>source_peak)source_peak=a;}
                float scaled=(float)v*gain;
                if(scaled>32767.f)scaled=32767.f;else if(scaled< -32768.f)scaled=-32768.f;
                v=(int16_t)scaled;memcpy(converted+to*2,&v,2);if(monitor && v)nonzero=1;
            }
        }
        if(p->type==4)controller_source_meter(source_peak,p->volume[0]);
        /* PC-speaker safety limiter for pad sounds (2026-11-06): the game mixes them very loud for the
         * controller's small speaker; on PC speakers a loud burst (first Neon absorb) was cut off at full
         * scale, which crackled painfully. Peaks are held at ISS_PAD_SPEAKER_PC_CEILING (default 0.3):
         * the gain drops at once when needed, ramped across the block (no click), and recovers slowly. */
        if (p->type==4 && !p->controller_device) {
            /* Bluetooth speaker: same limiter with a ceiling just under full scale (boosted Smoke/Video). */
            static float pc_ceiling=-1.f;
            if (pc_ceiling<0.f) {
                const char *e=getenv("ISS_PAD_SPEAKER_PC_CEILING");char *end=NULL;float v=e?strtof(e,&end):0.3f;
                pc_ceiling=(e && (end==e || *end)) || !(v>0.f && v<=1.f) ? 0.3f : v;
            }
            const float ceiling=(p->bt || p->bluetooth_speaker) ? 0.9f : pc_ceiling;
            float peak=0.f;
            for (size_t i=0;i<samples;++i) {
                float a;
                if (p->is_float) {float v;memcpy(&v,converted+i*4,4);a=v<0?-v:v;}
                else {int16_t v;memcpy(&v,converted+i*2,2);a=(v<0?-(float)v:(float)v)/32768.f;}
                if (a>peak)peak=a;
            }
            /* Clipped float input: measure the unclipped level from the source instead. */
            const float level=fmaxf(peak,source_peak*p->controller_gain);
            if (level>p->pc_loudest*1.25f && level>0.05f) {
                p->pc_loudest=level;
                printf("Controller feedback: loudest pad sound on %s so far %.2f (limited to %.2f)\n",
                    (p->bt || p->bluetooth_speaker) ? "the Bluetooth speaker" : "PC speakers",level,ceiling);
            }
            if (p->pc_limit<=0.f)p->pc_limit=1.f;
            const float start=p->pc_limit;
            float want=level>ceiling ? ceiling/level : 1.f;
            float end_gain=want<start ? want : fminf(1.f,start*1.15f+0.001f); /* back to full in ~0.5 s */
            if (end_gain>want)end_gain=want;
            const float begin=fminf(start,end_gain); /* never louder than needed at the start of the block */
            for (size_t f=0;f<(size_t)p->frames;++f) {
                const float g=begin+(end_gain-begin)*(float)f/(float)p->frames;
                for (int c=0;c<p->channels;++c) {
                    const size_t i=f*(size_t)p->channels+(size_t)c;
                    if (p->is_float) {float v;memcpy(&v,converted+i*4,4);v*=g;if(v>1.f)v=1.f;else if(v< -1.f)v=-1.f;memcpy(converted+i*4,&v,4);}
                    else {int16_t v;memcpy(&v,converted+i*2,2);v=(int16_t)((float)v*g);memcpy(converted+i*2,&v,2);}
                }
            }
            p->pc_limit=end_gain;
        }
        if (p->bt) {
            /* Mono (first two channels averaged) to the Bluetooth speaker; gain already applied. */
            float mono[2048];const int use=p->channels<2 ? 1 : 2;
            for (size_t f=0;f<(size_t)p->frames;++f) {
                float sum=0.f;
                for (int c=0;c<use;++c) {
                    const size_t at=f*(size_t)p->channels+(size_t)c;
                    if (p->is_float) {float v;memcpy(&v,converted+at*4,4);sum+=v;}
                    else {int16_t v;memcpy(&v,converted+at*2,2);sum+=(float)v/32768.f;}
                }
                mono[f]=sum/(float)use;
            }
            ds_bt_sink_push(p->bt,mono,p->frames);
        }
        if (p->type==4 && nonzero && !p->controller_signal_logged) {
            p->controller_signal_logged=1;puts("Controller feedback: first non-silent game pad-speaker buffer received");
        }
        if (p->downmix) {
            if (p->type==0) bytes=pc_mix_surround_stereo(converted,(size_t)p->frames,p->is_float,&p->main_mix);
            else bytes=downmix_stereo(converted,(size_t)p->frames,p->is_float);
        }
        if (p->controller_stereo) {
            /* Backward expansion keeps the mono guest buffer unchanged. */
            for (size_t f=(size_t)p->frames;f-->0;) {
                memcpy(converted+(f*2+1)*(size_t)p->sample_bytes,converted+f*(size_t)p->sample_bytes,(size_t)p->sample_bytes);
                memcpy(converted+(f*2)*(size_t)p->sample_bytes,converted+f*(size_t)p->sample_bytes,(size_t)p->sample_bytes);
            }
            bytes=(size_t)p->frames*2u*(size_t)p->sample_bytes;
        }
        if (p->dump) fwrite(converted,1,bytes,p->dump);
        if (p->stats) {
            uint64_t now=now_ns();
            if (!p->stat_start_ns) { p->stat_start_ns=now; p->stat_min_queued=1<<30; }
            else if (now-p->stat_last_ns>p->stat_max_gap_ns) p->stat_max_gap_ns=now-p->stat_last_ns;
            p->stat_last_ns=now; ++p->stat_buffers;
            if (p->stream) {
                int queued=SDL_GetAudioStreamQueued(p->stream);
                if (queued<p->stat_min_queued) p->stat_min_queued=queued;
                if (queued==0) ++p->stat_starved;
            }
            if (now-p->stat_start_ns>=5000000000u) {
                printf("Audio stats port %d: %u buffers/5s (expected %u), max gap %.2f ms, min queued %d bytes, starved %u\n",
                       (int)(p-ports),p->stat_buffers,(unsigned)(5u*48000u/(unsigned)p->frames),p->stat_max_gap_ns/1e6,
                       p->stat_min_queued,p->stat_starved);
                if (p->type==0 && p->downmix) {
                    printf("PC stereo mix: pre-limit peak %.3f, %u frames above full scale, %u invalid blocks suppressed, gain %.3f\n",
                           p->main_mix.peak,p->main_mix.over_full_scale,p->main_mix.invalid_blocks,p->main_mix.gain);
                    p->main_mix.peak=0.f;
                    p->main_mix.over_full_scale=0;
                    p->main_mix.invalid_blocks=0;
                }
                p->stat_start_ns=now; p->stat_max_gap_ns=0; p->stat_starved=0; p->stat_buffers=0; p->stat_min_queued=1<<30;
            }
        }
        if (pace) {
            uint64_t now=now_ns();
            if (!p->next_deadline_ns || now>p->next_deadline_ns+8*period) p->next_deadline_ns=now; /* start or stall: restart cadence */
            else sleep_until(p->next_deadline_ns);
            p->next_deadline_ns+=period;
        }

        if (p->bluetooth_speaker) {
            if (SDL_GetAudioStreamQueued(p->bluetooth_speaker->pcm)<8*(int)bytes) {
                SDL_PutAudioStreamData(p->bluetooth_speaker->pcm,converted,(int)bytes);
            }
        }
        if (p->stream) {
            /* The device drains the queue in quanta (21 ms on PipeWire), so the level
               is a sawtooth: its minimum over ~32 buffers is what gets controlled. */
            int low=2*(int)bytes, queued=SDL_GetAudioStreamQueued(p->stream);
            if (queued<(int)bytes) {
                static const unsigned char silence[2048*8*4];
                SDL_PutAudioStreamData(p->stream,silence,low-queued<(int)bytes ? low-queued : (int)bytes);
                queued=low;
            }
            if (!p->window_count || queued<p->window_min) p->window_min=queued;
            if (++p->window_count==32) {
                p->adjust_ns=p->window_min>low+2*(int)bytes ? (int64_t)(period/32) : p->window_min<low ? -(int64_t)(period/32) : 0;
                p->window_count=0;
            }
            p->next_deadline_ns+=(uint64_t)p->adjust_ns;
            SDL_PutAudioStreamData(p->stream,converted,(int)bytes);
        }
        ++buffers_out;
    }
    p->last_output_us=now_ns()/1000;
    pthread_mutex_unlock(&p->lock);
    return data ? (int32_t)samples : 0;
}
static ABI int32_t audio_output(int32_t handle, const void *data) { return output_port(handle,data,1); }
typedef struct { int32_t handle; const void *data; } OutputParam;
static ABI int32_t audio_outputs(const OutputParam *params, uint32_t count) {
    if (!params) return ERR_INVALID_POINTER;
    if (!count || count>PORTS) return ERR_PORT_FULL;
    int32_t result=0;
    /* One period per call for all ports (FMOD feeds main and BGM together): pacing every
       port would halve the rate and starve both. */
    for (uint32_t i=0;i<count;++i) { int32_t r=output_port(params[i].handle,params[i].data,i==0); if (r<0) return r; result=r; }
    return result;
}
static ABI int32_t audio_volume(int32_t handle, int32_t flags, const int32_t *volume) {
    int32_t error=0;
    pthread_mutex_lock(&table_lock);
    Port *p=port_of(handle,&error);
    pthread_mutex_unlock(&table_lock);
    if (!p) return error;
    if (!volume) return ERR_INVALID_POINTER;
    pthread_mutex_lock(&p->lock);
    for (int c=0;c<8;++c) if (flags & (1<<c)) {
        if (volume[c]<0 || volume[c]>VOLUME_0DB) { pthread_mutex_unlock(&p->lock); return ERR_INVALID_VOLUME; }
        p->volume[c]=volume[c];
    }
    pthread_mutex_unlock(&p->lock);
    return 0;
}
static ABI int32_t audio_state(int32_t handle, PortState *state) {
    int32_t error=0;
    pthread_mutex_lock(&table_lock);
    Port *p=port_of(handle,&error);
    pthread_mutex_unlock(&table_lock);
    if (!p) return error;
    if (!state) return ERR_INVALID_POINTER;
    memset(state,0,sizeof(*state));
    switch (p->type) {
    case 2: case 3: state->output=0x40; state->channel=1; break;          /* headphone */
    case 4: state->output=0x04; state->channel=1; state->volume=127; break; /* pad speaker */
    default: state->output=0x01; state->channel=(uint8_t)(p->channels>2 ? 2 : p->channels); break;
    }
    return 0;
}
static ABI int32_t audio_last_time(int32_t handle, uint64_t *time) {
    int32_t error=0;
    pthread_mutex_lock(&table_lock);
    Port *p=port_of(handle,&error);
    pthread_mutex_unlock(&table_lock);
    if (!p) return error;
    if (!time) return ERR_INVALID_POINTER;
    *time=p->last_output_us; return 0;
}

/* Host-side ports for other runtime libraries (Audio3d): same device, pacing and dump path as
   guest ports, without requiring the guest's sceAudioOutInit. */
int32_t runtime_audio_host_open(int32_t type, uint32_t frames, uint32_t format) { return open_port(type,frames,48000,format); }
int32_t runtime_audio_host_output(int32_t handle, const void *data, int pace) { return output_port(handle,data,pace); }
int32_t runtime_audio_host_close(int32_t handle) { return audio_close(handle); }

static const RuntimeExport exports[]={
    {"sceAudioOutInit",audio_init}, {"sceAudioOutOpen",audio_open}, {"sceAudioOutClose",audio_close},
    {"sceAudioOutOutput",audio_output}, {"sceAudioOutOutputs",audio_outputs},
    {"sceAudioOutSetVolume",audio_volume}, {"sceAudioOutGetPortState",audio_state},
    {"sceAudioOutGetLastOutputTime",audio_last_time},
};
uintptr_t runtime_audio_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_audio_report(void) { printf("Runtime: audio ports opened=%zu, buffers output=%zu\n",ports_opened,buffers_out); }
