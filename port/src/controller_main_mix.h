#ifndef ISS_CONTROLLER_MAIN_MIX_H
#define ISS_CONTROLLER_MAIN_MIX_H
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    float gain;
    float peak;
    unsigned over_full_scale;
    unsigned invalid_blocks;
    float previous[2];
    bool recovering;
} PcStereoLimiter;

static void pc_mix_store(unsigned char *sample, float value, int is_float) {
    if (is_float) memcpy(sample,&value,4);
    else {
        const int16_t pcm=(int16_t)(value*32767.f);
        memcpy(sample,&pcm,2);
    }
}

/* Keep the existing surround coefficients. Measure the stereo sum before
 * clipping, and lower both channels together only on peaks. */
static size_t pc_mix_surround_stereo(unsigned char *buffer, size_t frames,
                                     int is_float, PcStereoLimiter *limiter) {
    const float surround=0.70710678f;
    const size_t sample_bytes=is_float?4u:2u;
    float mix[2048][2];
    float peak=0.f;
    bool invalid=false;
    for (size_t frame=0;frame<frames;++frame) {
        float channel[8];
        for (size_t c=0;c<8;++c) {
            const unsigned char *sample=buffer+(frame*8+c)*sample_bytes;
            if (is_float) {
                memcpy(&channel[c],sample,4);
                if (!isfinite(channel[c])) {
                    invalid=true;
                    channel[c]=0.f;
                } else if (fabsf(channel[c])>4.f) {
                    /* Several times the nominal PCM range is invalid at the
                     * final mix input. Do not normalize this noise to 0.9. */
                    invalid=true;
                }
            } else {
                int16_t value;
                memcpy(&value,sample,2);
                channel[c]=value/32768.f;
            }
        }
        const float left=channel[0]+surround*(channel[2]+channel[4]+channel[6]);
        const float right=channel[1]+surround*(channel[2]+channel[5]+channel[7]);
        mix[frame][0]=isfinite(left)?left:0.f;
        mix[frame][1]=isfinite(right)?right:0.f;
        const float level=fmaxf(fabsf(mix[frame][0]),fabsf(mix[frame][1]));
        peak=fmaxf(peak,level);
        limiter->over_full_scale+=level>1.f;
    }
    limiter->peak=fmaxf(limiter->peak,peak);
    const size_t ramp=frames<144?frames:144;
    if (invalid) {
        ++limiter->invalid_blocks;
        for (size_t frame=0;frame<frames;++frame) {
            const float gain=frame<ramp?1.f-(float)(frame+1)/(float)ramp:0.f;
            for (size_t c=0;c<2;++c)
                pc_mix_store(buffer+(frame*2+c)*sample_bytes,limiter->previous[c]*gain,is_float);
        }
        limiter->previous[0]=limiter->previous[1]=0.f;
        limiter->recovering=true;
        return frames*2*sample_bytes;
    }
    const float ceiling=0.90f;
    const float target=peak>ceiling?ceiling/peak:1.f;
    const float previous=limiter->gain>0.f?limiter->gain:1.f;
    /* Recover over about 200 ms after a transient. Attack applies before
     * writing any sample so the first frame cannot exceed the ceiling. */
    const float end=fminf(target,previous+(float)frames/9600.f);
    const float begin=fminf(previous,end);
    for (size_t frame=0;frame<frames;++frame) {
        float gain=begin+(end-begin)*(float)frame/(float)frames;
        if (limiter->recovering && frame<ramp) gain*=(float)frame/(float)ramp;
        for (size_t c=0;c<2;++c) {
            unsigned char *sample=buffer+(frame*2+c)*sample_bytes;
            const float value=mix[frame][c]*gain;
            pc_mix_store(sample,value,is_float);
            if (frame+1==frames) limiter->previous[c]=value;
        }
    }
    limiter->recovering=false;
    limiter->gain=end;
    return frames*2*sample_bytes;
}
#endif
