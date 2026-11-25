/* The Opus settings the Bluetooth speaker sink uses (src/dualsense_bt_sink.h) give exactly one
 * 200-byte frame per 480 samples (the size report 0x36 reserves), and the frame decodes back to
 * the same tone. Needs libopus, no controller. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <opus/opus.h>
#include "../src/dualsense_bt_speaker.h"

int main(void) {
    int error=0;
    OpusEncoder *enc=opus_encoder_create(48000,2,OPUS_APPLICATION_RESTRICTED_CELT,&error);
    assert(enc && error==OPUS_OK);
    assert(opus_encoder_ctl(enc,OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS))==OPUS_OK);
    assert(opus_encoder_ctl(enc,OPUS_SET_BITRATE(DS_BT_OPUS_BYTES*8*100))==OPUS_OK);
    assert(opus_encoder_ctl(enc,OPUS_SET_VBR(0))==OPUS_OK);
    assert(opus_encoder_ctl(enc,OPUS_SET_COMPLEXITY(5))==OPUS_OK);
    OpusDecoder *dec=opus_decoder_create(48000,2,&error);
    assert(dec && error==OPUS_OK);

    float pcm[DS_BT_OPUS_FRAMES*2], back[DS_BT_OPUS_FRAMES*2];
    unsigned char frame[DS_BT_OPUS_BYTES];
    double in_energy=0, out_energy=0;
    const double pi=3.14159265358979323846;
    for (int k=0;k<50;++k) {
        for (int i=0;i<DS_BT_OPUS_FRAMES;++i) {
            const float v=0.5f*(float)sin(2*pi*1000.0*(k*DS_BT_OPUS_FRAMES+i)/48000.0);
            pcm[i*2]=v; pcm[i*2+1]=v;
        }
        const int n=opus_encode_float(enc,pcm,DS_BT_OPUS_FRAMES,frame,DS_BT_OPUS_BYTES);
        assert(n==DS_BT_OPUS_BYTES);                     /* CBR: always the full block */
        uint8_t report[DS_BT_SPEAKER_REPORT_SIZE];
        ds_bt_speaker_report(report,(unsigned)k,(uint8_t)k,frame,(size_t)n);
        const int got=opus_decode_float(dec,report+79,DS_BT_OPUS_BYTES,back,DS_BT_OPUS_FRAMES,0);
        assert(got==DS_BT_OPUS_FRAMES);
        if (k>=10) for (int i=0;i<DS_BT_OPUS_FRAMES*2;++i) { in_energy+=pcm[i]*pcm[i]; out_energy+=back[i]*back[i]; }
    }
    /* The decoder lags by its look-ahead, so compare levels rather than samples. */
    const double ratio=out_energy/in_energy;
    printf("dualsense bluetooth opus: 200-byte CBR frames, decoded level ratio %.3f\n",ratio);
    assert(ratio>0.8 && ratio<1.25);
    opus_encoder_destroy(enc); opus_decoder_destroy(dec);
    return 0;
}
