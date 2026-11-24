#ifndef ISS_CONTROLLER_GAIN_H
#define ISS_CONTROLLER_GAIN_H
#include <stdlib.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
static volatile LONG *controller_gain_state;
#endif
static float controller_gain_parse(const char *text) {
    char *end=NULL;float gain=text?strtof(text,&end):2.f;
    if ((text && (end==text || *end)) || !(gain>=0.f && gain<=8.f))return 2.f;
    return gain;
}
/* Windows live adjustment uses one process-owned memory page, created at audio
 * startup. Each buffer does one atomic read: no file I/O, polling thread,
 * controller lock or SDL reconfiguration in the audio hot path. */
static float controller_gain_live(void) {
#ifdef _WIN32
    static HANDLE mapping;static int attempted;
    static float fallback;
    if (!attempted) {
        attempted=1;fallback=controller_gain_parse(getenv("ISS_PAD_SPEAKER_GAIN"));
        wchar_t name[96];swprintf(name,96,L"Local\\ISSControllerSpeaker-%lu",GetCurrentProcessId());
        mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,16,name);
        if (mapping)controller_gain_state=(volatile LONG*)MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,16);
        if (controller_gain_state) {InterlockedExchange(&controller_gain_state[1],(LONG)(fallback*1000.f));InterlockedExchange(&controller_gain_state[0],0x49535341);}
    }
    volatile LONG *state=controller_gain_state;
    if (state) {
        LONG gain=InterlockedCompareExchange(&state[1],0,0);
        if (gain>=0 && gain<=8000)return gain/1000.f;
    }
    return fallback;
#else
    return controller_gain_parse(getenv("ISS_PAD_SPEAKER_GAIN"));
#endif
}
static void controller_source_meter(float peak,int volume) {
#ifdef _WIN32
    if (!controller_gain_state)return;
    if (!(peak>=0.f))peak=0.f;
    if(peak>1.f)peak=1.f;
    LONG level=(LONG)(peak*1000000.f),previous=InterlockedCompareExchange(&controller_gain_state[2],0,0);
    while(level>previous) {
        LONG actual=InterlockedCompareExchange(&controller_gain_state[2],level,previous);
        if(actual==previous)break;
        previous=actual;
    }
    InterlockedExchange(&controller_gain_state[3],volume);
#else
    (void)peak;(void)volume;
#endif
}
#endif
