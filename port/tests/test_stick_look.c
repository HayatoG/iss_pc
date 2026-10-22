/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <assert.h>
#include "../src/runtime_pad.c"

static TlgGpuInputSettings input;
static int capture;
uint64_t tlggpu_total_draws(void) { return 0; }
void tlggpu_frame_counters(TlgGpuFrameCounters *out) { memset(out,0,sizeof(*out)); }
void tlggpu_mouse_take(TlgGpuMouse *m) { memset(m,0,sizeof(*m)); }
void tlggpu_input_settings(TlgGpuInputSettings *out) { *out=input; }
void tlggpu_mouse_pad_hud(int a,float x,float y,int t,int c) { (void)a;(void)x;(void)y;(void)t;(void)c; }
int tlggpu_overlay_captures_input(void) { return capture; }
void tlggpu_overlay_notice(const char *s,float t) { (void)s;(void)t; }
void tlggpu_graffiti_guide(int force) { (void)force; }
float tlggpu_sensor_aim_scale(void) { return 1; }
uintptr_t runtime_lookup(const RuntimeExport *t,size_t n,const char *s) { (void)t;(void)n;(void)s;return 0; }

int main(void) {
    for (int v=0;v<256;++v) {
        assert(stick_look_axis((uint8_t)v,1)==v);
        assert(stick_look_axis((uint8_t)v,0)==v);
        assert(stick_look_axis((uint8_t)v,NAN)==v);
    }
    assert(stick_look_axis(128,3)==128);
    assert(stick_look_axis(0,3)==0 && stick_look_axis(255,3)==255);
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER,"dummy");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6190");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    SDL_VirtualJoystickDesc desc; SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;desc.vendor_id=0x1d50;desc.product_id=0x6190;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    const SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);assert(id);
    gamepad=gamepads[0]=SDL_OpenGamepad(id);assert(gamepad);
    SDL_Joystick *j=SDL_GetGamepadJoystick(gamepad);
    assert(SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-32768));
    assert(SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768));
    assert(SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_RIGHTX,16384));
    assert(SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_LEFTX,16384));
    SDL_UpdateGamepads(); PadData original,d;
    input.stick_look_sensitivity=1;sample_host(&original);
    input.stick_look_sensitivity=3;sample_host(&d);
    assert(d.right_x>original.right_x && d.left_x==original.left_x && d.right_y==128);
    assert(SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,32767));
    SDL_UpdateGamepads();sample_host(&d);assert(d.right_x==original.right_x);
    assert(SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-32768));
    assert(SDL_SetJoystickVirtualAxis(j,SDL_GAMEPAD_AXIS_RIGHTX,32767));
    SDL_UpdateGamepads();sample_host(&d);assert(d.right_x==255);
    input.stick_look_sensitivity=1;sample_host(&d);assert(d.right_x==255);
    capture=1;sample_host(&d);assert(d.right_x==128);
    assert(SDL_DetachVirtualJoystick(id));SDL_UpdateGamepads();SDL_Quit();
    puts("PASS: free-look gain, neutral/default identity, physical stick, L2 bypass and unchanged full deflection");
}
