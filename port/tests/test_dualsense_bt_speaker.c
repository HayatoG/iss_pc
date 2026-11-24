/* Bluetooth DualSense speaker packets (src/dualsense_bt_speaker.h): CRC, report 0x36 layout,
 * the audio-route effects block and the 512 -> 480 framing. No controller or SDL needed. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/dualsense_bt_speaker.h"

static int emits;
static float last[DS_BT_OPUS_FRAMES*2];
static void capture(void *context, const float stereo[DS_BT_OPUS_FRAMES*2]) {
    (void)context; ++emits; memcpy(last,stereo,sizeof(last));
}
static void set_flag(const char *value) {
#ifdef _WIN32
    _putenv_s("ISS_PAD_SPEAKER_BLUETOOTH",value ? value : "");
#else
    if (value) setenv("ISS_PAD_SPEAKER_BLUETOOTH",value,1); else unsetenv("ISS_PAD_SPEAKER_BLUETOOTH");
#endif
}
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }

int main(void) {
    /* zlib CRC-32 check value. */
    assert(ds_bt_crc32_update(0,(const uint8_t *)"123456789",9)==0xCBF43926u);
    /* After the 0xA2 header alone the CRC is 0xEADA2D49: the "0xA2 seed" constant SAxense and
     * DS5Dongle start from, so prefixing 0xA2 and seeding are the same computation. */
    assert(ds_bt_crc32(NULL,0)==0xEADA2D49u);
    const uint8_t prefixed[3]={0xA2,0x36,0x10};
    assert(ds_bt_crc32(prefixed+1,2)==ds_bt_crc32_update(0,prefixed,3));

    /* Report 0x36. */
    uint8_t opus[DS_BT_OPUS_BYTES+20];
    for (int i=0;i<(int)sizeof(opus);++i) opus[i]=(uint8_t)(i*7+1);
    uint8_t r[DS_BT_SPEAKER_REPORT_SIZE];
    ds_bt_speaker_report(r,0x13,0x5A,opus,DS_BT_OPUS_BYTES);
    assert(r[0]==0x36 && r[1]==0x30);                       /* sequence wraps to 4 bits, high nibble */
    assert(r[2]==0x91 && r[3]==7 && r[4]==0xFE && r[10]==0x5A);
    for (int i=5;i<=9;++i) assert(r[i]==64);
    assert(r[11]==0x92 && r[12]==64);
    for (int i=13;i<=76;++i) assert(r[i]==0);                /* silent haptics */
    assert(r[77]==0x93 && r[78]==DS_BT_OPUS_BYTES);
    assert(!memcmp(r+79,opus,DS_BT_OPUS_BYTES));
    for (int i=279;i<394;++i) assert(r[i]==0);
    assert(le32(r+394)==ds_bt_crc32(r,394));
    /* A short frame is zero padded; an oversized one is cut at 200 bytes, never past the block. */
    ds_bt_speaker_report(r,1,0,opus,12);
    assert(r[1]==0x10 && !memcmp(r+79,opus,12) && r[79+12]==0 && r[78]==DS_BT_OPUS_BYTES);
    ds_bt_speaker_report(r,2,0,opus,sizeof(opus));
    assert(!memcmp(r+79,opus,DS_BT_OPUS_BYTES) && r[279]==0 && le32(r+394)==ds_bt_crc32(r,394));
    /* A changed byte changes the CRC (the pad drops reports with a bad one). */
    const uint32_t good=le32(r+394); r[100]^=1; assert(ds_bt_crc32(r,394)!=good);

    /* Audio-route effects block: only audio bits are valid. */
    uint8_t e[47];
    ds_bt_audio_effect(e,1);
    assert(e[0]==0xA0 && e[1]==0x80 && e[5]==0x64 && e[7]==0x30 && e[37]==0x02);
    for (int i=0;i<47;++i) if (i!=0 && i!=1 && i!=5 && i!=7 && i!=37) assert(e[i]==0);
    ds_bt_audio_effect(e,0);
    assert(e[0]==0x80 && e[1]==0x80 && e[7]==0 && e[37]==0);

    /* Framing: one Opus frame per 512 source samples, remainder kept for the next push. */
    DsBtFramer f={0};
    float ramp[1000];
    for (int i=0;i<1000;++i) ramp[i]=(float)(i%512)/511.f*2.f-1.f;   /* -1 .. +1 over each 512 */
    assert(ds_bt_framer_push(&f,ramp,511,capture,NULL)==0 && emits==0);
    assert(ds_bt_framer_push(&f,ramp+511,489,capture,NULL)==1 && emits==1 && f.count==488);
    assert(last[0]==-1.f && last[1]==-1.f);                   /* first sample kept */
    assert(fabsf(last[(DS_BT_OPUS_FRAMES-1)*2]-1.f)<1e-6f);   /* last sample kept */
    for (int o=0;o<DS_BT_OPUS_FRAMES;++o) {
        assert(last[o*2]==last[o*2+1]);                       /* mono on both channels */
        if (o) assert(last[o*2]>last[(o-1)*2]);              /* the ramp stays monotonic */
    }
    /* Out-of-range input is clamped. */
    DsBtFramer g={0}; float loud[512];
    for (int i=0;i<512;++i) loud[i]=i&1 ? 3.f : -3.f;
    assert(ds_bt_framer_push(&g,loud,512,capture,NULL)==1);
    for (int o=0;o<DS_BT_OPUS_FRAMES*2;++o) assert(last[o]>=-1.f && last[o]<=1.f);
    /* One second of game audio is 93.75 reports, one per 10.667 ms. */
    DsBtFramer h={0}; static float second[48000];
    assert(ds_bt_framer_push(&h,second,48000,NULL,NULL)==93 && h.count==48000-93*512);
    assert(DS_BT_PERIOD_NS_X3/3u==10666666u);

    /* On by default; only ISS_PAD_SPEAKER_BLUETOOTH=0 turns it off. */
    set_flag(NULL); assert(dualsense_bt_speaker_requested());
    set_flag("0"); assert(!dualsense_bt_speaker_requested());
    set_flag("1"); assert(dualsense_bt_speaker_requested());

    puts("dualsense bluetooth speaker: ok");
    return 0;
}
