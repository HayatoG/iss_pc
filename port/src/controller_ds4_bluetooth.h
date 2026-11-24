#ifndef ISS_CONTROLLER_DS4_BLUETOOTH_H
#define ISS_CONTROLLER_DS4_BLUETOOTH_H
#include <SDL3/SDL.h>
#include <SDL3/SDL_hidapi.h>
#ifndef ISS_DS4_BLUETOOTH
/* Built without libsbc (ISS_DS4_BLUETOOTH undefined): the DualShock 4 Bluetooth speaker route is left
 * out; the same state is kept so runtime_audio.c compiles, and ds4_bt_open never opens a route. */
typedef struct {
    SDL_AudioStream *pcm;
    SDL_AtomicInt failed;
    SDL_Mutex *effects_lock;
    Uint8 low_motor, high_motor, rgb[3], effects_dirty;
    Uint64 rumble_until;
} Ds4BluetoothSpeaker;
static inline Ds4BluetoothSpeaker *ds4_bt_open(SDL_Gamepad *pad,const SDL_AudioSpec *input) {
    (void)pad;(void)input;return NULL;
}
static inline void ds4_bt_close(Ds4BluetoothSpeaker *speaker) { (void)speaker; }
#else
#include <sbc/sbc.h>
#include <stdio.h>
#include <stdlib.h>

enum { DS4_BT_PCM_BYTES=2048, DS4_BT_FRAME_BYTES=109, DS4_BT_POLL_MS=4 };
typedef struct {
    SDL_hid_device *hid;
    SDL_AudioStream *pcm;
    SDL_Thread *thread;
    SDL_AtomicInt stop, failed;
    sbc_t codec;
    Uint16 sequence;
    SDL_Mutex *effects_lock;
    Uint8 low_motor, high_motor, rgb[3], effects_dirty;
    Uint64 rumble_until;
    unsigned effects_sent;
} Ds4BluetoothSpeaker;

static bool ds4_bt_write(Ds4BluetoothSpeaker *speaker, Uint8 *report, size_t size) {
    const Uint8 prefix=0xa2;
    const Uint32 crc=SDL_crc32(SDL_crc32(0,&prefix,1),report,size-4);
    for (int i=0;i<4;++i) report[size-4+i]=(Uint8)(crc>>(8*i));
    return SDL_hid_write(speaker->hid,report,size)>0;
}

/* Sony audio control and 16 ms speaker reports, as documented by DS4Windows. */
static bool ds4_bt_arm(Ds4BluetoothSpeaker *speaker,bool enabled) {
    Uint8 report[78]={0x11,0xc0|DS4_BT_POLL_MS,enabled?0xa0:0,0x80};
    report[24]=enabled?0x4f:0;
    return ds4_bt_write(speaker,report,sizeof(report));
}

static bool ds4_bt_effects(Ds4BluetoothSpeaker *speaker) {
    Uint8 report[78]={0x11,0xc0|DS4_BT_POLL_MS,0xa0,0x80};
    SDL_LockMutex(speaker->effects_lock);
    if (speaker->rumble_until && SDL_GetTicksNS()>=speaker->rumble_until) {
        speaker->low_motor=speaker->high_motor=0;
        speaker->rumble_until=0;
        speaker->effects_dirty|=1;
    }
    const Uint8 dirty=speaker->effects_dirty;
    speaker->effects_dirty=0;
    report[3]|=dirty;
    report[6]=speaker->high_motor;
    report[7]=speaker->low_motor;
    SDL_memcpy(report+8,speaker->rgb,3);
    SDL_UnlockMutex(speaker->effects_lock);
    report[24]=0x4f;
    if (!dirty) return true;
    ++speaker->effects_sent;
    return ds4_bt_write(speaker,report,sizeof(report));
}

static bool ds4_bt_send(Ds4BluetoothSpeaker *speaker,const Uint8 pcm[DS4_BT_PCM_BYTES]) {
    Uint8 report[462]={0x17,0x40|DS4_BT_POLL_MS,0xa0,0,0,0x02};
    report[3]=(Uint8)speaker->sequence;
    report[4]=(Uint8)(speaker->sequence>>8);
    for (int i=0;i<4;++i) {
        ssize_t written=0;
        if (sbc_encode(&speaker->codec,pcm+i*512,512,report+6+i*DS4_BT_FRAME_BYTES,
                       DS4_BT_FRAME_BYTES,&written)!=512 || written!=DS4_BT_FRAME_BYTES)
            return false;
    }
    speaker->sequence+=4;
    return ds4_bt_write(speaker,report,sizeof(report));
}

static int SDLCALL ds4_bt_worker(void *context) {
    Ds4BluetoothSpeaker *speaker=context;
    const Uint64 period=16000000;
    bool buffered=false;
    const bool stats=getenv("TLG_AUDIO_STATS")!=NULL;
    unsigned sent=0, underruns=0, trims=0, late=0;
    Uint64 worst_write=0;
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);
    /* Give the receiver a lead before switching to the steady report cadence. */
    const Uint8 silence[DS4_BT_PCM_BYTES]={0};
    for (int i=0;i<5 && !SDL_GetAtomicInt(&speaker->stop);++i) {
        if (!ds4_bt_send(speaker,silence)) {
            fprintf(stderr,"Controller feedback: DualShock 4 Bluetooth priming failed: %s\n",SDL_GetError());
            SDL_SetAtomicInt(&speaker->failed,1);
            return 0;
        }
    }
    Uint64 deadline=SDL_GetTicksNS()+period;
    SDL_DelayPrecise(period);
    while (!SDL_GetAtomicInt(&speaker->stop)) {
        Uint8 pcm[DS4_BT_PCM_BYTES]={0};
        const int available=SDL_GetAudioStreamAvailable(speaker->pcm);
        if (available<0) break;
        if (available>=2*DS4_BT_PCM_BYTES) buffered=true;
        if (available<DS4_BT_PCM_BYTES && buffered) {
            ++underruns;
            buffered=false;
        }
        if (available>4*DS4_BT_PCM_BYTES) {
            /* Cap latency if Windows stalls the HID writer. */
            SDL_GetAudioStreamData(speaker->pcm,pcm,sizeof(pcm));
            ++trims;
        }
        const int count=buffered?SDL_GetAudioStreamData(speaker->pcm,pcm,sizeof(pcm)):0;
        if (count<0) break;
        SDL_memset(pcm+count,0,sizeof(pcm)-(size_t)count);
        const Uint64 writing=SDL_GetTicksNS();
        if (!ds4_bt_effects(speaker) || !ds4_bt_send(speaker,pcm)) break;
        const Uint64 now=SDL_GetTicksNS();
        if (now-writing>worst_write) worst_write=now-writing;
        deadline+=period;
        if (deadline>now) SDL_DelayPrecise(deadline-now);
        else {
            if (now-deadline>period/2) deadline=now;
            ++late;
        }
        if (++sent==312) {
            if (stats) fprintf(stderr,"DS4 Bluetooth audio: %u source underruns, %u trims, %u late reports, %u combined effects, max write %.2f ms\n",
                               underruns,trims,late,speaker->effects_sent,worst_write/1e6);
            sent=underruns=trims=late=0;
            worst_write=0;
            speaker->effects_sent=0;
        }
    }
    if (!SDL_GetAtomicInt(&speaker->stop)) {
        fprintf(stderr,"Controller feedback: DualShock 4 Bluetooth speaker stopped: %s\n",SDL_GetError());
        SDL_SetAtomicInt(&speaker->failed,1);
    }
    return 0;
}

static void ds4_bt_close(Ds4BluetoothSpeaker *speaker) {
    SDL_SetAtomicInt(&speaker->stop,1);
    SDL_WaitThread(speaker->thread,NULL);
    ds4_bt_arm(speaker,false);
    SDL_hid_close(speaker->hid);
    SDL_DestroyAudioStream(speaker->pcm);
    SDL_DestroyMutex(speaker->effects_lock);
    sbc_finish(&speaker->codec);
    SDL_free(speaker);
}

static Ds4BluetoothSpeaker *ds4_bt_open(SDL_Gamepad *pad,const SDL_AudioSpec *input) {
    if (!pad || SDL_GetGamepadVendor(pad)!=0x054c ||
        (SDL_GetGamepadProduct(pad)!=0x09cc && SDL_GetGamepadProduct(pad)!=0x05c4) ||
        SDL_GetGamepadConnectionState(pad)!=SDL_JOYSTICK_CONNECTION_WIRELESS)
        return NULL;
    const char *path=SDL_GetGamepadPath(pad);
    if (!path || !*path) return NULL;
    Ds4BluetoothSpeaker *speaker=SDL_calloc(1,sizeof(*speaker));
    if (!speaker) return NULL;
    if (sbc_init(&speaker->codec,0)!=0) {
        SDL_free(speaker);
        return NULL;
    }
    speaker->codec.frequency=SBC_FREQ_32000;
    speaker->codec.blocks=SBC_BLK_16;
    speaker->codec.subbands=SBC_SB_8;
    speaker->codec.mode=SBC_MODE_JOINT_STEREO;
    speaker->codec.allocation=SBC_AM_SNR;
    speaker->codec.bitpool=48;
    speaker->codec.endian=SBC_LE;
    const SDL_AudioSpec output={SDL_AUDIO_S16LE,2,32000};
    speaker->pcm=SDL_CreateAudioStream(input,&output);
    speaker->effects_lock=SDL_CreateMutex();
    speaker->hid=SDL_hid_open_path(path);
    if (speaker->pcm && speaker->effects_lock && speaker->hid && ds4_bt_arm(speaker,true))
        speaker->thread=SDL_CreateThread(ds4_bt_worker,"DS4 Bluetooth speaker",speaker);
    if (speaker->thread) return speaker;
    if (speaker->hid) {
        ds4_bt_arm(speaker,false);
        SDL_hid_close(speaker->hid);
    }
    SDL_DestroyAudioStream(speaker->pcm);
    SDL_DestroyMutex(speaker->effects_lock);
    sbc_finish(&speaker->codec);
    SDL_free(speaker);
    return NULL;
}
#endif /* ISS_DS4_BLUETOOTH */
#endif
