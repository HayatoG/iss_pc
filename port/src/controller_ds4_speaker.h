#ifndef ISS_CONTROLLER_DS4_SPEAKER_H
#define ISS_CONTROLLER_DS4_SPEAKER_H
#include <SDL3/SDL.h>
#include <SDL3/SDL_hidapi.h>

static bool ds4_usb_speaker_supported(SDL_Gamepad *pad) {
    return pad && SDL_GetGamepadVendor(pad)==0x054c &&
           SDL_GetGamepadProduct(pad)==0x09cc &&
           SDL_GetGamepadConnectionState(pad)==SDL_JOYSTICK_CONNECTION_WIRED;
}

/* The second-generation wired DS4 exposes stereo USB audio. E0 routes
 * the right channel to the built-in speaker (03) or restores the jack (00).
 * Use the active gamepad's HID path, not the first matching Sony device. */
static bool ds4_usb_speaker(SDL_Gamepad *pad,bool enabled) {
    if (!ds4_usb_speaker_supported(pad))return false;
    const char *path=SDL_GetGamepadPath(pad);
    if (!path || !*path)return false;
    SDL_hid_device *hid=SDL_hid_open_path(path);
    if (!hid)return false;
    const unsigned char route[]={0xe0,0x00,enabled?0x03:0x00};
    const bool ok=SDL_hid_send_feature_report(hid,route,sizeof(route))==(int)sizeof(route);
    if (ok && enabled) {
        /* Volume update only: leave rumble and light bar valid bits clear. */
        const unsigned char volume[32]={[0]=0x05,[1]=0x80,[22]=0x4f,[23]=0x85};
        SDL_hid_write(hid,volume,sizeof(volume));
    }
    SDL_hid_close(hid);
    return ok;
}
#endif
