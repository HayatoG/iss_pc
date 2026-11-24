/* Controller-speaker endpoint selection, separate from ordinary game audio. */
#ifndef ISS_CONTROLLER_AUDIO_H
#define ISS_CONTROLLER_AUDIO_H
#include <SDL3/SDL.h>
#include <string.h>
static int controller_audio_name(const char *name) {
    return name && (SDL_strcasestr(name,"Wireless Controller") || SDL_strcasestr(name,"DualSense") ||
                    SDL_strcasestr(name,"DualShock") || SDL_strcasestr(name,"PS5 Controller") ||
                    SDL_strcasestr(name,"PS4 Controller"));
}
/* A name is deliberately required to match exactly. Ambiguous auto/name matches
 * fall back rather than playing private audio through an arbitrary device. */
static SDL_AudioDeviceID controller_audio_device(const char *choice, int *dedicated, int *muted) {
    *dedicated=0;*muted=0;
    if (choice && !SDL_strcasecmp(choice,"none")) {*muted=1;return 0;}
    if (choice && (!SDL_strcasecmp(choice,"default") || !strcmp(choice,"0")))return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    const int automatic=!choice || !*choice || !SDL_strcasecmp(choice,"auto");
    int count=0,matches=0;SDL_AudioDeviceID selected=0;
    SDL_AudioDeviceID *devices=SDL_GetAudioPlaybackDevices(&count);
    for (int i=0;devices && i<count;++i) {
        const char *name=SDL_GetAudioDeviceName(devices[i]);
        if (name && (automatic ? controller_audio_name(name) : !strcmp(choice,name))) {selected=devices[i];++matches;}
    }
    SDL_free(devices);
    if (matches==1) {*dedicated=1;return selected;}
    fprintf(stderr,"Controller feedback: %s speaker endpoint %s; using normal PC output\n",automatic?"controller":"requested",matches?"ambiguous":"not found");
    return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
}
static int controller_audio_speaker_map(SDL_AudioStream *stream) {
    SDL_AudioSpec output;
    if (!SDL_GetAudioStreamFormat(stream,NULL,&output) || output.channels<1 || output.channels>32)return 0;
    int map[32];
    for (int i=0;i<output.channels;++i)map[i]=i<2?i:-1;
    return SDL_SetAudioStreamOutputChannelMap(stream,map,output.channels);
}
#endif
