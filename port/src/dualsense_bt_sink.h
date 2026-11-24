/* Bluetooth DualSense speaker sink (opt-in prototype, ISS_PAD_SPEAKER_BLUETOOTH=1).
 * The game's pad-speaker samples are framed (dualsense_bt_speaker.h), Opus-encoded and written as
 * HID output report 0x36 through a second SDL_hid handle; SDL's joystick driver keeps its own
 * handle for input, LED and rumble (Windows opens HID devices shared read/write).
 * A private thread sends one report per 10.667 ms on its own clock: bursts from the game's audio
 * thread are absorbed by a short queue, extra frames beyond it are dropped (bounded latency).
 * Built only with ISS_HAVE_OPUS; otherwise ds_bt_sink_open() always returns NULL. */
#ifndef ISS_DUALSENSE_BT_SINK_H
#define ISS_DUALSENSE_BT_SINK_H
#include <SDL3/SDL.h>
#include <stdio.h>
#include "dualsense_bt_speaker.h"

typedef struct DsBtSink DsBtSink;
#ifdef ISS_HAVE_OPUS
#include <opus/opus.h>
#define DS_BT_QUEUE 8      /* frames held at most (~85 ms) */
#define DS_BT_START 2      /* frames buffered before the first send (~21 ms) */
struct DsBtSink {
    SDL_hid_device *hid;
    OpusEncoder *encoder;
    SDL_Mutex *lock;
    SDL_Thread *thread;
    DsBtFramer framer;
    float queue[DS_BT_QUEUE][DS_BT_OPUS_FRAMES*2];
    int head, count, started;
    SDL_AtomicInt stop;
    unsigned dropped, failures;
};
static inline void ds_bt_sink_enqueue(void *context, const float stereo[DS_BT_OPUS_FRAMES*2]) {
    DsBtSink *s=(DsBtSink *)context; /* called with s->lock held */
    if (s->count==DS_BT_QUEUE) {s->head=(s->head+1)%DS_BT_QUEUE;--s->count;++s->dropped;}
    memcpy(s->queue[(s->head+s->count)%DS_BT_QUEUE],stereo,sizeof(s->queue[0]));
    ++s->count;
}
static inline int ds_bt_sink_thread(void *argument) {
    DsBtSink *s=(DsBtSink *)argument;
    const Uint64 start=SDL_GetTicksNS();
    Uint64 tick=0;unsigned sequence=0;uint8_t counter=0;
    while (!SDL_GetAtomicInt(&s->stop)) {
        const Uint64 deadline=start+tick*DS_BT_PERIOD_NS_X3/3u, now=SDL_GetTicksNS();
        if (now<deadline) SDL_DelayPrecise(deadline-now);
        else if (now-deadline>100000000u) tick=(now-start)*3u/DS_BT_PERIOD_NS_X3; /* stalled: resync */
        ++tick;
        float pcm[DS_BT_OPUS_FRAMES*2];int have=0;
        SDL_LockMutex(s->lock);
        if (!s->started && s->count>=DS_BT_START) s->started=1;
        if (s->started && s->count) {
            memcpy(pcm,s->queue[s->head],sizeof(pcm));s->head=(s->head+1)%DS_BT_QUEUE;--s->count;have=1;
        } else if (s->started) s->started=0; /* underrun: rebuffer instead of stuttering */
        SDL_UnlockMutex(s->lock);
        if (!have) continue;
        unsigned char opus[DS_BT_OPUS_BYTES];
        const opus_int32 n=opus_encode_float(s->encoder,pcm,DS_BT_OPUS_FRAMES,opus,DS_BT_OPUS_BYTES);
        if (n<0) continue;
        uint8_t report[DS_BT_SPEAKER_REPORT_SIZE];
        ds_bt_speaker_report(report,sequence++,counter++,opus,(size_t)n);
        if (SDL_hid_write(s->hid,report,sizeof(report))<0) {
            if (++s->failures==1 || s->failures==100)
                fprintf(stderr,"Controller feedback: Bluetooth speaker report failed (%u): %s\n",s->failures,SDL_GetError());
        } else if (s->failures) {printf("Controller feedback: Bluetooth speaker writes resumed\n");s->failures=0;}
    }
    return 0;
}
/* Exactly one Bluetooth DualSense (or DualSense Edge) must be present; ambiguity falls back. */
static inline DsBtSink *ds_bt_sink_open(void) {
    SDL_hid_device_info *list=SDL_hid_enumerate(0x054c,0);
    char *path=NULL;int matches=0;
    for (SDL_hid_device_info *d=list;d;d=d->next)
        if ((d->product_id==0x0ce6 || d->product_id==0x0df2) && d->bus_type==SDL_HID_API_BUS_BLUETOOTH && d->path) {
            if (!path || strcmp(path,d->path)) {++matches;SDL_free(path);path=SDL_strdup(d->path);}
        }
    SDL_hid_free_enumeration(list);
    if (matches!=1) {
        printf("Controller feedback: Bluetooth speaker: %s DualSense over Bluetooth\n",matches ? "more than one" : "no");
        SDL_free(path);return NULL;
    }
    DsBtSink *s=(DsBtSink *)SDL_calloc(1,sizeof(*s));
    int error=0;
    if (s) s->hid=SDL_hid_open_path(path);
    SDL_free(path);
    if (s && s->hid) s->encoder=opus_encoder_create(48000,2,OPUS_APPLICATION_RESTRICTED_CELT,&error); /* as DS5Dongle */
    if (s && s->encoder) {
        opus_encoder_ctl(s->encoder,OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
        opus_encoder_ctl(s->encoder,OPUS_SET_BITRATE(DS_BT_OPUS_BYTES*8*100));
        opus_encoder_ctl(s->encoder,OPUS_SET_VBR(0));
        opus_encoder_ctl(s->encoder,OPUS_SET_COMPLEXITY(5));
        s->lock=SDL_CreateMutex();
    }
    if (s && s->lock) s->thread=SDL_CreateThread(ds_bt_sink_thread,"ds-bt-speaker",s);
    if (!s || !s->thread) {
        fprintf(stderr,"Controller feedback: Bluetooth speaker unavailable (%s)\n",
                !s ? "out of memory" : !s->hid ? SDL_GetError() : !s->encoder ? opus_strerror(error) : SDL_GetError());
        if (s) {
            if (s->lock) SDL_DestroyMutex(s->lock);
            if (s->encoder) opus_encoder_destroy(s->encoder);
            if (s->hid) SDL_hid_close(s->hid);
            SDL_free(s);
        }
        return NULL;
    }
    puts("Controller feedback: Bluetooth DualSense speaker stream started (report 0x36, Opus 48 kHz)");
    return s;
}
static inline void ds_bt_sink_push(DsBtSink *s, const float *mono, int n) {
    if (!s) return;
    SDL_LockMutex(s->lock);
    ds_bt_framer_push(&s->framer,mono,n,ds_bt_sink_enqueue,s);
    SDL_UnlockMutex(s->lock);
}
static inline void ds_bt_sink_close(DsBtSink *s) {
    if (!s) return;
    SDL_SetAtomicInt(&s->stop,1);
    SDL_WaitThread(s->thread,NULL);
    if (s->dropped) printf("Controller feedback: Bluetooth speaker dropped %u late frames\n",s->dropped);
    SDL_DestroyMutex(s->lock);
    opus_encoder_destroy(s->encoder);
    SDL_hid_close(s->hid);
    SDL_free(s);
}
#else
static inline DsBtSink *ds_bt_sink_open(void) {
    puts("Controller feedback: Bluetooth speaker not built into this version (no Opus)");
    return NULL;
}
static inline void ds_bt_sink_push(DsBtSink *s, const float *mono, int n) {(void)s;(void)mono;(void)n;}
static inline void ds_bt_sink_close(DsBtSink *s) {(void)s;}
#endif
#endif
