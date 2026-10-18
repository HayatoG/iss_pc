/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <assert.h>
#include "../src/runtime_pad.c"

static TlgGpuInputSettings input;
static int capture;
void tlggpu_mouse_take(TlgGpuMouse *m) { memset(m,0,sizeof(*m)); }
void tlggpu_input_settings(TlgGpuInputSettings *out) { *out=input; }
void tlggpu_mouse_pad_hud(int a,float x,float y,int t,int c) { (void)a;(void)x;(void)y;(void)t;(void)c; }
int tlggpu_overlay_captures_input(void) { return capture; }
void tlggpu_overlay_notice(const char *s,float t) { (void)s;(void)t; }
void tlggpu_graffiti_guide(int force) { (void)force; }
float tlggpu_sensor_aim_scale(void) { return 1; }
uintptr_t runtime_lookup(const RuntimeExport *t,size_t n,const char *s) { (void)t;(void)n;(void)s;return 0; }
static bool SDLCALL sensors(void *data,bool on) { (void)data;(void)on;return true; }

static SDL_Joystick *joystick;
static PadData read_sample(float pitch,float yaw,float roll) {
    const float gyro[3]={pitch,yaw,roll}, accel[3]={0,9.80665f,0};
    SDL_UpdateGamepads();
    if (SDL_GamepadHasSensor(gamepad,SDL_SENSOR_ACCEL))
        assert(SDL_SendJoystickVirtualSensorData(joystick,SDL_SENSOR_ACCEL,SDL_GetTicksNS(),accel,3));
    assert(SDL_SendJoystickVirtualSensorData(joystick,SDL_SENSOR_GYRO,SDL_GetTicksNS(),gyro,3));
    SDL_UpdateGamepads(); /* virtual sensor reports are committed by the joystick update */
    SDL_Delay(2);
    PadData d;
    sample_host(&d);
    return d;
}

int main(void) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6190");
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER,"dummy");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    const SDL_VirtualJoystickSensorDesc descriptions[]={{SDL_SENSOR_GYRO,200},{SDL_SENSOR_ACCEL,200}};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id=0x1d50;desc.product_id=0x6190;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.nsensors=2;desc.sensors=descriptions;desc.SetSensorsEnabled=sensors;
    desc.name="gyro aim test controller";
    const SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id);
    gamepad=gamepads[0]=SDL_OpenGamepad(id);
    assert(gamepad);
    joystick=SDL_GetGamepadJoystick(gamepad);
    assert(SDL_SetGamepadSensorEnabled(gamepad,SDL_SENSOR_GYRO,true));
    assert(SDL_SetGamepadSensorEnabled(gamepad,SDL_SENSOR_ACCEL,true));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-32768));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-32768));
    input=(TlgGpuInputSettings){.gyro_focused=1,.gyro_sensitivity=1};
    PadData d=read_sample(0,1,0);
    assert(d.right_x==128 && d.right_y==128); /* default off */
    input.gyro_aim=1;
    d=read_sample(0,1,0);
    assert(d.right_x==128); /* trigger not held */
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,32767));
    d=read_sample(0,1,0);
    assert((d.buttons & BTN_L2) && d.right_x<128 && d.right_y==128);
    assert(d.angular_velocity[1]==1); /* real graffiti sensor data is not rewritten */
    d=read_sample(1,0,0);
    assert(d.right_y<128 && d.right_x==128);
    input.gyro_invert_y=1;
    d=read_sample(1,0,0);
    assert(d.right_y>128);
    input.gyro_invert_y=0;
    d=read_sample(0,0,1);
    assert(d.right_x==128 && d.right_y==128); /* roll is not camera yaw */
    input.gyro_focused=0;
    d=read_sample(1,1,0);
    assert(d.right_x==128 && d.right_y==128);
    input.gyro_focused=1;
    capture=1;
    d=read_sample(1,1,0);
    assert(d.right_x==128 && d.right_y==128 && !d.buttons);
    capture=0;
    read_sample(0,0,0);
    motion.mode=1;
    d=read_sample(1,1,0);
    assert(d.right_x==128 && d.right_y==128); /* graffiti mode owns the sticks */
    motion.mode=0;
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_BACK,true));
    d=read_sample(1,1,0);
    assert(d.right_x==128 && d.right_y==128); /* touchpad modifier suppresses aim */
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_BACK,false));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHTX,32767));
    d=read_sample(0,-1,0);
    assert(d.right_x==255); /* physical stick + gyro saturates */
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-32768));
    d=read_sample(1,1,0);
    assert(d.right_x==255 && d.right_y==128); /* release keeps the physical stick, removes gyro */
    assert(SDL_DetachVirtualJoystick(id));
    SDL_UpdateGamepads();
    sample_host(&d);
    assert(d.right_x==128 && d.right_y==128); /* disconnect clears the gyro and physical stick */
    desc.nsensors=1; /* a gyro without an accelerometer is sufficient for aiming */
    const SDL_JoystickID gyro_only=SDL_AttachVirtualJoystick(&desc);
    assert(gyro_only);
    gamepad=gamepads[0]=SDL_OpenGamepad(gyro_only);
    assert(gamepad && !SDL_GamepadHasSensor(gamepad,SDL_SENSOR_ACCEL));
    joystick=SDL_GetGamepadJoystick(gamepad);
    assert(SDL_SetGamepadSensorEnabled(gamepad,SDL_SENSOR_GYRO,true));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFT_TRIGGER,32767));
    read_sample(0,1,0);
    d=read_sample(0,1,0);
    assert(d.right_x<128 && d.right_y==128);
    assert(SDL_DetachVirtualJoystick(gyro_only));
    SDL_UpdateGamepads();
    sample_host(&d);
    SDL_Quit();
    puts("PASS: SDL virtual gyro through pad, L2 gate, focus/menu, graffiti and physical stick");
    return 0;
}
