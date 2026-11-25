#undef NDEBUG
#include <assert.h>
#include "../src/runtime_audio.c"

void compat_sleep_ns(uint64_t ns) { SDL_DelayNS(ns); }
void runtime_pad_controller_speaker(int enabled) { (void)enabled; }
void runtime_power_wheel_status(IssPowerWheelStatus *status) {
    memset(status,0,sizeof(*status));
    status->supported=status->ready=1;
    status->active=1;
}
int tlggpu_pad_speaker_enabled(void) { return 1; }
uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    (void)table; (void)count; (void)name;
    abort();
}

static void check_main_mix(void) {
    float mix[64][8]={0};
    PcStereoLimiter limiter={0};
    mix[0][0]=0.4f;
    mix[0][1]=0.2f;
    mix[0][2]=0.2f;
    mix[0][3]=1.f; /* LFE was excluded by the existing mix. */
    assert(pc_mix_surround_stereo((unsigned char *)mix,64,1,&limiter)==64*2*sizeof(float));
    assert(fabsf(mix[0][0]-(0.4f+0.2f*0.70710678f))<0.00001f);
    assert(fabsf(mix[0][1]-(0.2f+0.2f*0.70710678f))<0.00001f);
    assert(limiter.gain==1.f && limiter.over_full_scale==0);

    memset(mix,0,sizeof(mix));
    for (int f=0;f<64;++f) {
        mix[f][0]=1.f;
        mix[f][1]=0.5f;
        mix[f][2]=mix[f][4]=mix[f][6]=1.f;
    }
    pc_mix_surround_stereo((unsigned char *)mix,64,1,&limiter);
    const float left=1.f+3.f*0.70710678f;
    const float right=0.5f+0.70710678f;
    assert(limiter.over_full_scale==64 && fabsf(limiter.peak-left)<0.00001f);
    assert(fabsf(mix[0][0]-0.9f)<0.00001f);
    assert(fabsf(mix[0][1]-0.9f*right/left)<0.00001f);
    const float old_gain=limiter.gain;
    memset(mix,0,sizeof(mix));
    mix[0][0]=1.124486e7f;
    pc_mix_surround_stereo((unsigned char *)mix,64,1,&limiter);
    assert(limiter.invalid_blocks==1 && limiter.recovering && limiter.gain==old_gain);
    assert(mix[15][6]==0.f && mix[15][7]==0.f); /* last stereo frame */
    memset(mix,0,sizeof(mix));
    for (int f=0;f<64;++f) mix[f][0]=0.1f;
    pc_mix_surround_stereo((unsigned char *)mix,64,1,&limiter);
    assert(!limiter.recovering && mix[0][0]==0.f && mix[15][6]>0.f);
    assert(limiter.gain>old_gain && limiter.gain<1.f);
    memset(mix,0,sizeof(mix));
    mix[0][0]=NAN;
    pc_mix_surround_stereo((unsigned char *)mix,64,1,&limiter);
    assert(limiter.invalid_blocks==2 && limiter.recovering);
}

int main(void) {
    check_main_mix();
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(),"SDL_AUDIODRIVER","dummy",true);
    assert(SDL_Init(SDL_INIT_AUDIO));
    assert(_putenv_s("ISS_PAD_SPEAKER_GAIN","4.0")==0);
    assert(_putenv_s("ISS_PAD_SPEAKER_DEVICE","default")==0);
    assert(audio_init()==0);
    float input[1024];
    for (int i=0;i<1024;++i) input[i]=10.f;
    int32_t handle=open_port(4,1024,48000,3);
    assert(handle>0);
    Port *port=&ports[handle&255];
    assert(!port->controller_device && !port->bluetooth_speaker);
    port->dump=tmpfile();
    assert(port->dump);
    assert(output_port(handle,input,0)==1024);
    assert(port->controller_gain==0.5f);
    rewind(port->dump);
    float result[1024];
    assert(fread(result,sizeof(float),1024,port->dump)==1024);
    for (int i=0;i<1024;++i) assert(isfinite(result[i]) && fabsf(result[i])<=0.300001f);
    assert(input[0]==10.f);
    assert(audio_close(handle)==0);

    assert(_putenv_s("ISS_PAD_SPEAKER_DEVICE","none")==0);
    handle=open_port(4,1024,48000,3);
    assert(handle>0);
    port=&ports[handle&255];
    Ds4BluetoothSpeaker speaker={0};
    SDL_AudioSpec spec={SDL_AUDIO_F32,1,48000};
    speaker.pcm=SDL_CreateAudioStream(&spec,&spec);
    speaker.effects_lock=SDL_CreateMutex();
    assert(speaker.pcm && speaker.effects_lock);
    port->bluetooth_speaker=&speaker;
    bluetooth_route=&speaker;
    bluetooth_route_id=123;
    assert(!runtime_audio_bluetooth_rumble(456,65535,65535,100));
    assert(runtime_audio_bluetooth_rumble(123,65535,32768,100));
    assert(speaker.low_motor==255 && speaker.high_motor==128);
    const uint8_t rgb[]={7,17,27};
    assert(runtime_audio_bluetooth_led(123,rgb));
    assert(!memcmp(rgb,speaker.rgb,3) && speaker.effects_dirty==3);
    for (int i=0;i<1024;++i) input[i]=0.1f;
    assert(output_port(handle,input,0)==1024);
    assert(port->controller_gain==1.f);
    assert(SDL_GetAudioStreamData(speaker.pcm,result,sizeof(result))==sizeof(result));
    for (int i=0;i<1024;++i) assert(fabsf(result[i]-0.1f)<0.000001f);
    port->bluetooth_speaker=NULL;
    bluetooth_route=NULL;
    bluetooth_route_id=0;
    SDL_DestroyAudioStream(speaker.pcm);
    SDL_DestroyMutex(speaker.effects_lock);
    assert(audio_close(handle)==0);

    handle=open_port(0,1024,48000,3);
    assert(handle>0 && ports[handle&255].controller_gain==1.f);
    assert(audio_close(handle)==0);
    SDL_Quit();
    puts("PASS: current-build PC fallback limiter, main gain, DS4 unity gain, route ownership, rumble and RGB updates");
}
