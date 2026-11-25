/* USB DualSense audio-only output payload. SDL supplies report ID/transport.
 * Layout reference: SDL_hidapi_ps5.c and nowrep/dualsensectl main.c.
 * Valid bits deliberately exclude motors, triggers, LEDs and microphone volume. */
#ifndef ISS_DUALSENSE_SPEAKER_H
#define ISS_DUALSENSE_SPEAKER_H
#include <SDL3/SDL.h>
#include <string.h>
static void dualsense_speaker_payload(unsigned char effect[47],int enabled) {
    memset(effect,0,47);
    effect[0]=(enabled?0x20:0)|0x80; /* speaker volume and audio routing only */
    effect[5]=0x64;      /* known 100% speaker setting used by Linux driver */
    effect[7]=enabled?0x20:0; /* internal speaker on; headset routing retained */
}
static int dualsense_usb_speaker(SDL_Gamepad *pad,int enabled) {
    if (!pad || SDL_GetGamepadVendor(pad)!=0x054c ||
        (SDL_GetGamepadProduct(pad)!=0x0ce6 && SDL_GetGamepadProduct(pad)!=0x0df2) ||
        SDL_GetGamepadConnectionState(pad)!=SDL_JOYSTICK_CONNECTION_WIRED)return 0;
    unsigned char effect[47];dualsense_speaker_payload(effect,enabled);
    return SDL_SendGamepadEffect(pad,effect,sizeof(effect));
}
/* Bluetooth prototype (ISS_PAD_SPEAKER_BLUETOOTH=1): the same switch as a 0x31 effects report.
 * Only reached when the Bluetooth sink in runtime_audio.c opened and asked for the speaker. */
#include "dualsense_bt_speaker.h"
static int dualsense_bt_speaker(SDL_Gamepad *pad,int enabled) {
    if (!pad || !dualsense_bt_speaker_requested() || SDL_GetGamepadVendor(pad)!=0x054c ||
        (SDL_GetGamepadProduct(pad)!=0x0ce6 && SDL_GetGamepadProduct(pad)!=0x0df2) ||
        SDL_GetGamepadConnectionState(pad)!=SDL_JOYSTICK_CONNECTION_WIRELESS)return 0;
    unsigned char effect[47];ds_bt_audio_effect(effect,enabled);
    return SDL_SendGamepadEffect(pad,effect,sizeof(effect));
}
#endif
