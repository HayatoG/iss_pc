/* DualSense internal speaker over Bluetooth: pure packet building, no SDL, no Opus, no I/O.
 * On by default (ISS_PAD_SPEAKER_BLUETOOTH=0 turns it off). Sony has not published this format; the layout
 * follows two independent public reverse-engineering projects that agree on it:
 *   awalol/DS5Dongle src/audio.cpp (single-frame report 0x36, 398 bytes) and
 *   ebraminio/LinuxAudio4Dualsense5 src/ds5_membrane_sink.c (same report, size and CRC).
 * Over Bluetooth the pad has no audio endpoint: each output report 0x36 carries one 10 ms Opus
 * frame (48 kHz stereo, CBR 200 bytes) in a 0x13 sub-packet. The pad plays one report per
 * 512/48000 s (10.667 ms), so 512 source samples are resampled to the 480 an Opus frame holds.
 * The audio-route switch is sent separately as an ordinary 47-byte effects block, which SDL
 * wraps in its own Bluetooth report 0x31 (with CRC) like any LED or rumble change. */
#ifndef ISS_DUALSENSE_BT_SPEAKER_H
#define ISS_DUALSENSE_BT_SPEAKER_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DS_BT_SPEAKER_REPORT_ID    0x36
#define DS_BT_SPEAKER_REPORT_SIZE  398   /* report ID included, CRC in the last 4 bytes */
#define DS_BT_OPUS_BYTES           200   /* CBR 160 kbit/s, one 10 ms frame */
#define DS_BT_SOURCE_FRAMES        512   /* 48 kHz samples per report period */
#define DS_BT_OPUS_FRAMES          480   /* samples per Opus frame */
#define DS_BT_PERIOD_NS_X3         32000000ull /* 512/48000 s = 32,000,000/3 ns */

/* On by default (2026-11-06, user-tested on a Bluetooth DualSense): it only engages when exactly one
 * DualSense/Edge is connected over Bluetooth and no USB speaker endpoint exists. ISS_PAD_SPEAKER_BLUETOOTH=0
 * turns it off. */
static inline int dualsense_bt_speaker_requested(void) {
    const char *v=getenv("ISS_PAD_SPEAKER_BLUETOOTH");
    return !v || strcmp(v,"0");
}

/* zlib CRC-32 (reflected 0xEDB88320). */
static inline uint32_t ds_bt_crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    crc=~crc;
    for (size_t i=0;i<len;++i) {
        crc^=data[i];
        for (int b=0;b<8;++b) crc=(crc>>1)^(0xEDB88320u&(0u-(crc&1u)));
    }
    return ~crc;
}
/* Bluetooth output reports are checked over the HIDP header byte 0xA2 followed by the report
 * (report ID first); the result is stored little-endian in the report's last 4 bytes. */
static inline uint32_t ds_bt_crc32(const uint8_t *report, size_t len) {
    const uint8_t header=0xA2;
    return ds_bt_crc32_update(ds_bt_crc32_update(0,&header,1),report,len);
}
static inline void ds_bt_seal(uint8_t *report, size_t size) {
    const uint32_t crc=ds_bt_crc32(report,size-4);
    for (int i=0;i<4;++i) report[size-4+(size_t)i]=(uint8_t)(crc>>(8*i));
}

/* One speaker report (DS5Dongle "single" layout):
 *   [0] 0x36  [1] sequence<<4
 *   [2] 0x91 (0x11|0x80, sized) [3] 7  [4] 0xFE (microphone off)  [5..9] 64  [10] counter
 *   [11] 0x92 (0x12 haptics) [12] 64  [13..76] haptics, silent here
 *   [77] 0x93 (0x13 internal speaker) [78] 200  [79..278] Opus frame, zero padded
 *   [279..393] zero  [394..397] CRC-32 */
static inline void ds_bt_speaker_report(uint8_t report[DS_BT_SPEAKER_REPORT_SIZE], unsigned sequence, uint8_t counter,
                                 const uint8_t *opus, size_t opus_len) {
    memset(report,0,DS_BT_SPEAKER_REPORT_SIZE);
    report[0]=DS_BT_SPEAKER_REPORT_ID;
    report[1]=(uint8_t)((sequence&0x0Fu)<<4);
    report[2]=0x11|0x80; report[3]=7; report[4]=0xFE;
    for (int i=5;i<=9;++i) report[i]=64;
    report[10]=counter;
    report[11]=0x12|0x80; report[12]=64;
    report[77]=0x13|0x80; report[78]=DS_BT_OPUS_BYTES;
    if (opus && opus_len) memcpy(report+79,opus,opus_len>DS_BT_OPUS_BYTES ? DS_BT_OPUS_BYTES : opus_len);
    ds_bt_seal(report,DS_BT_SPEAKER_REPORT_SIZE);
}

/* 47-byte effects block (the common part after the Bluetooth header) for SDL_SendGamepadEffect.
 * Offsets as in Linux hid-playstation's dualsense_output_report_common:
 *   [0] valid_flag0: 0x80 audio control, 0x20 speaker volume   [1] valid_flag1: 0x80 audio control 2
 *   [5] speaker volume  [7] audio control (0x30 = output to the internal speaker)
 *   [37] audio control 2 (0x02 = speaker preamp, as LinuxAudio4Dualsense5 sends it)
 * Motors, triggers, LEDs and microphone are left untouched (their valid bits stay clear). */
static inline void ds_bt_audio_effect(uint8_t effect[47], int enabled) {
    memset(effect,0,47);
    effect[0]=(uint8_t)(0x80|(enabled ? 0x20 : 0));
    effect[1]=0x80;
    effect[5]=0x64;
    effect[7]=enabled ? 0x30 : 0x00;
    effect[37]=enabled ? 0x02 : 0x00;
}

/* Collects mono 48 kHz samples; every 512 it emits 480 stereo frames (left = right), linearly
 * resampled so the first and last samples are kept exactly. */
typedef struct { float in[DS_BT_SOURCE_FRAMES]; int count; } DsBtFramer;
typedef void (*DsBtEmit)(void *context, const float stereo[DS_BT_OPUS_FRAMES*2]);
static inline int ds_bt_framer_push(DsBtFramer *f, const float *mono, int n, DsBtEmit emit, void *context) {
    int emitted=0;
    for (int i=0;i<n;++i) {
        float v=mono[i];
        f->in[f->count++]=v>1.f ? 1.f : v< -1.f ? -1.f : v;
        if (f->count<DS_BT_SOURCE_FRAMES) continue;
        float out[DS_BT_OPUS_FRAMES*2];
        for (int o=0;o<DS_BT_OPUS_FRAMES;++o) {
            const double pos=(double)o*(DS_BT_SOURCE_FRAMES-1)/(DS_BT_OPUS_FRAMES-1);
            int k=(int)pos; if (k>=DS_BT_SOURCE_FRAMES-1) k=DS_BT_SOURCE_FRAMES-2;
            const float t=(float)(pos-k), s=f->in[k]+(f->in[k+1]-f->in[k])*t;
            out[o*2]=s; out[o*2+1]=s;
        }
        f->count=0; ++emitted;
        if (emit) emit(context,out);
    }
    return emitted;
}
#endif
