#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "../src/runtime_pad.c"
// runtime_mouse_aim.c (the native aim hook) needs the guest; pad tests run without it.
int runtime_mouse_aim_installed(void) { return 0; }
int runtime_power_wheel_gameplay(void) { return 1; }
void runtime_power_wheel_status(IssPowerWheelStatus *out){memset(out,0,sizeof(*out));}
void runtime_power_wheel_request(int index){(void)index;}
void runtime_power_wheel_visual(int open,int choice){(void)open;(void)choice;}
void tlggpu_power_wheel_settings(int *enabled,int keys[4],int *instant){*enabled=0;*instant=0;memset(keys,0,4*sizeof(*keys));}
static int direct_available,motion_calls;
static float observed_dx,observed_dy;
int runtime_mouse_aim_motion(float dx, float dy, float sensitivity, int invert_y, int active) { (void)sensitivity; (void)invert_y; if(active){motion_calls++;observed_dx=dx;observed_dy=dy;} return direct_available; }
#ifdef _WIN32
#define setenv(name, value, overwrite) _putenv_s(name, value)
#endif

static int capture;
/* The GPU library (not linked here): the mouse and the input settings. */
static TlgGpuMouse supplied_mouse;
static TlgGpuInputSettings supplied_settings;
void tlggpu_mouse_take(TlgGpuMouse *mouse) { *mouse=supplied_mouse; }
void tlggpu_mouse_pad_hud(int a, float x, float y, int t, int c) { (void)a; (void)x; (void)y; (void)t; (void)c; }
void tlggpu_input_settings(TlgGpuInputSettings *settings) { *settings=supplied_settings; }
uint64_t tlggpu_total_draws(void) { return 0; }
void tlggpu_frame_counters(TlgGpuFrameCounters *out) { memset(out,0,sizeof(*out)); }
int tlggpu_overlay_captures_input(void) { return capture; }
/* The GPU library (not linked here): on-screen notices, the graffiti guide, the menu's aim speed. */
void tlggpu_overlay_notice(const char *text, float seconds) { (void)text; (void)seconds; }
void tlggpu_graffiti_guide(int force) { (void)force; }
float tlggpu_sensor_aim_scale(void) { return 1.0f; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static void inject(const char *path, const char *tokens) {
    FILE *f=fopen(path,"w");
    assert(f);
    fputs(tokens,f);
    fclose(f);
    usleep(25000);
}
static PadData read_pad(void) {
    PadData d;
    assert(pad_read_state(1,&d)==0);
    return d;
}
/* One finger in the middle of the left (480) or right (1440) half of the touchpad. */
static int touch_at(const PadData *d, int x) {
    return d->touch_count==1 && d->touches[0].x==x && d->touches[0].y==471;
}
/* Two fingers at the left and right ends of the touchpad. */
static int two_ends(const PadData *d) {
    return d->touch_count==2 && d->touches[0].x==120 && d->touches[1].x==1800 && d->touches[0].id!=d->touches[1].id;
}
static void pump(void) { SDL_UpdateJoysticks(); SDL_UpdateGamepads(); }
/* The virtual controller's sensors. */
static bool SDLCALL on_sensors(void *userdata, bool enabled) { (void)userdata; (void)enabled; return true; }
/* Rumble commands that start the motors (SDL's own stop when a duration ends is not counted). */
static int rumbles;
static uint16_t rumble_low, rumble_high;
static bool SDLCALL on_rumble(void *userdata, Uint16 low, Uint16 high) {
    (void)userdata;
    if (low || high) { ++rumbles; rumble_low=low; rumble_high=high; }
    return true;
}

int main(void) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR); /* a failed assert prints, no dialog */
#endif
    const char *tmp=getenv("TMPDIR") ? getenv("TMPDIR") : getenv("TEMP") ? getenv("TEMP") : "/tmp";
    char path[1024];
    snprintf(path,sizeof(path),"%s/tlg-pad-test-XXXXXX",tmp);
    int fd=mkstemp(path);
    assert(fd>=0);
    close(fd);
    setenv("TLG_PAD_FILE",path,1);
    setenv("SDL_VIDEODRIVER","dummy",1);
    /* Only the virtual test controller is a gamepad, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);
    /* Direct mouse motion cannot alter physical controller axes, including
     * when the direct hook is unavailable. Motion is sent only to direct aim. */
    supplied_settings.mouse_enabled=1;supplied_settings.sensitivity=1;
    supplied_mouse.captured=1;supplied_mouse.dx=120;supplied_mouse.dy=-80;
    for(int available=0;available<2;available++){
        direct_available=available;
        PadData d={0};d.right_x=76;d.right_y=201;d.timestamp=1000000;
        sample_mouse(&d,false,NULL);d.timestamp+=16000;sample_mouse(&d,false,NULL);
        assert(d.right_x==76&&d.right_y==201&&observed_dx==120&&observed_dy==-80);
    }
    assert(motion_calls==4);memset(&supplied_mouse,0,sizeof(supplied_mouse));memset(&supplied_settings,0,sizeof(supplied_settings));

    /* A click without a touch surface (Back, Tab, Backspace, the pad file) plays like a finger:
     * the touch comes first, the button CLICK_LEAD_US later, and the touch stays CLICK_TAIL_US
     * after the release. God of War III opens photo mode on the release of a left click with
     * the finger still down; a touch that appears and vanishes with the button is ignored. */
    inject(path,"cross l3 touchpad_left");
    PadData data=read_pad();
    assert((data.buttons & (BTN_CROSS|BTN_L3))==(BTN_CROSS|BTN_L3));
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,480));
    usleep(60000);
    data=read_pad();
    assert((data.buttons & BTN_TOUCHPAD) && touch_at(&data,480));
    usleep(60000);
    inject(path,"");
    data=read_pad();
    assert(data.buttons==0 && touch_at(&data,480)); /* released: the finger lingers */
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);

    /* A tap shorter than the lead still clicks, for CLICK_HOLD_US. */
    inject(path,"touchpad_right");
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    inject(path,"");
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    usleep(60000);
    data=read_pad();
    assert((data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    usleep(60000);
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);

    /* L1 + the right click is a left click (photo mode on a pad without a touchpad). The side
     * is chosen when the click starts and kept until it ends. */
    inject(path,"l1 touchpad_right");
    data=read_pad();
    assert((data.buttons & BTN_L1) && touch_at(&data,480));
    inject(path,"touchpad_right");
    data=read_pad();
    assert(!(data.buttons & BTN_L1) && touch_at(&data,480));
    inject(path,"");
    data=read_pad(); /* the release is seen here: the tail starts */
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);

    /* The settings menu takes the input and ends a click; the source starts a new one after. */
    inject(path,"touchpad_right");
    usleep(60000);
    data=read_pad();
    assert(touch_at(&data,1440));
    capture=1;
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);
    capture=0;
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    inject(path,"");
    data=read_pad(); /* the release is seen here: the tail starts */
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);

    /* Two fingers at both ends (inFAMOUS: lifting the car off Reggie): both click sources, or
     * the right click + R1. The fingers come first, the button CLICK_LEAD_US later, R1 is hidden. */
    inject(path,"touchpad_left touchpad_right");
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && two_ends(&data));
    usleep(60000);
    data=read_pad();
    assert((data.buttons & BTN_TOUCHPAD) && two_ends(&data));
    inject(path,"");
    data=read_pad(); /* the release is seen here: the fingers linger */
    assert(!(data.buttons & BTN_TOUCHPAD) && two_ends(&data));
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);
    inject(path,"r1 touchpad_right");
    data=read_pad();
    assert(data.buttons==0 && two_ends(&data));
    usleep(60000);
    data=read_pad();
    assert(data.buttons==BTN_TOUCHPAD && two_ends(&data));
    inject(path,"r1");
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && two_ends(&data)); /* R1 stays hidden during the tail */
    inject(path,"");
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);

    SDL_VirtualJoystickTouchpadDesc touch={.nfingers=2};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="tlg test controller";
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    desc.ntouchpads=1;
    desc.touchpads=&touch;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);
    assert(joystick);
    /* A real touchpad: the fingers and the click go to the game as they are. */
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.75f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,true,0.25f,1.0f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    pump();
    data=read_pad();
    assert(gamepad && data.touch_count==2 && (data.buttons & BTN_TOUCHPAD));
    assert(data.touches[0].x==1439 && data.touches[0].y==471 && data.touches[1].x==480 && data.touches[1].y==942);
    /* DualShock 4 touch IDs: a new number (1..127) for each new finger, kept while it stays down;
     * the held time counts from the first finger of the touch. */
    const uint8_t first=data.touches[0].id, second=data.touches[1].id;
    assert(first>=1 && first<=127 && second>=1 && second<=127 && first!=second);
    usleep(20000);
    data=read_pad();
    assert(data.touches[0].id==first && data.touches[1].id==second && data.touch_held_time>=20000);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,false,0,0,0));
    pump();
    data=read_pad();
    assert(data.touch_count==1 && data.touches[0].id==first);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,true,0.25f,1.0f,1.0f));
    pump();
    data=read_pad();
    assert(data.touch_count==2 && data.touches[0].id==first && data.touches[1].id!=first && data.touches[1].id!=second);
    assert(data.touch_held_time>=20000);
    capture=1;
    data=read_pad();
    assert(data.touch_count==0 && data.buttons==0 && data.touch_held_time==0);
    capture=0;
    /* The click without a finger is played as a left click. */
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,false,0,0,0));
    pump();
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,480));
    usleep(60000);
    data=read_pad();
    assert((data.buttons & BTN_TOUCHPAD) && touch_at(&data,480));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,false));
    pump();
    data=read_pad();
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);
    /* Back (Share, View) is the right click: the pause menu. With L1 it is the left click. */
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_BACK,true));
    pump();
    data=read_pad();
    assert(!(data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    usleep(60000);
    data=read_pad();
    assert((data.buttons & BTN_TOUCHPAD) && touch_at(&data,1440));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_BACK,false));
    pump();
    data=read_pad();
    usleep(220000);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,true));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_BACK,true));
    pump();
    data=read_pad();
    assert((data.buttons & BTN_L1) && !(data.buttons & BTN_TOUCHPAD) && touch_at(&data,480));
    usleep(60000);
    data=read_pad();
    assert((data.buttons & (BTN_L1|BTN_TOUCHPAD))==(BTN_L1|BTN_TOUCHPAD) && touch_at(&data,480));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,false));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_BACK,false));
    pump();
    data=read_pad();
    usleep(220000);
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);
    /* Two touchpads (Steam Controller 2026, Steam Deck): the left one is the left half of the PS4
     * touchpad, the right one the right half; the right one's click (misc2) clicks it. */
    SDL_CloseJoystick(joystick);
    assert(SDL_DetachVirtualJoystick(id));
    pump();
    data=read_pad();
    assert(!gamepad);
    SDL_VirtualJoystickTouchpadDesc trackpads[2]={{.nfingers=1},{.nfingers=1}};
    desc.ntouchpads=2;
    desc.touchpads=trackpads;
    SDL_VirtualJoystickSensorDesc sensors[2]={{SDL_SENSOR_GYRO,250.f},{SDL_SENSOR_ACCEL,250.f}};
    desc.nsensors=2;
    desc.sensors=sensors;
    desc.SetSensorsEnabled=on_sensors;
    desc.Rumble=on_rumble;
    id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    joystick=SDL_OpenJoystick(id);
    assert(joystick);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.5f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,1,0,true,0.5f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_MISC2,true));
    pump();
    data=read_pad();
    assert(gamepad && data.touch_count==2 && (data.buttons & BTN_TOUCHPAD));
    assert(data.touches[0].x==480 && data.touches[1].x==1439 && data.touches[0].id!=data.touches[1].id);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,1,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_MISC2,false));
    pump();
    data=read_pad();
    assert(data.buttons==0 && data.touch_count==0);

    /* Trackpad haptics: a tick when a thumb lands, a pulse when it slides 2.5% of the trackpad
     * (45 ms apart at most); none while the game rumbles. SDL skips a rumble equal to one still
     * running, so the tick of the touch above ends first. */
    supplied_settings.rumble=1;
    usleep(50000);
    pump();
    rumbles=0;
    assert(SDL_SetJoystickVirtualTouchpad(joystick,1,0,true,0.5f,0.5f,1.0f));
    pump();
    data=read_pad();
    assert(rumbles==1 && rumble_low==1800 && rumble_high==7000);
    usleep(50000);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,1,0,true,0.53f,0.5f,1.0f));
    pump();
    data=read_pad();
    assert(rumbles==2 && rumble_low==4200 && rumble_high==11000);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,1,0,false,0,0,0));
    pump();
    data=read_pad();
    static const uint8_t game_rumble[2]={100,0}, motors_off[2]={0,0};
    assert(pad_vibration(1,game_rumble)==0);
    const int with_game=rumbles;
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.5f,0.5f,1.0f));
    pump();
    data=read_pad();
    assert(data.touch_count==1 && rumbles==with_game);
    assert(pad_vibration(1,motors_off)==0);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    pump();
    data=read_pad();
    supplied_settings.rumble=0;

    /* One round stick dead zone (0.10): a slow diagonal push past it moves on both axes (a zone
     * per axis would hold both at the centre); a push inside it reads centred, a full one full. */
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFTX,2621));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFTY,2621));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHTX,1638));
    pump();
    data=read_pad();
    assert(data.left_x>128 && data.left_y>128 && data.left_x==data.left_y);
    assert(data.right_x==128 && data.right_y==128);
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFTX,32767));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFTY,0));
    pump();
    data=read_pad();
    assert(data.left_x==255 && data.left_y==128);
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_LEFTX,0));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHTX,0));
    pump();

    /* scePadResetOrientation: the pose the controller has now becomes the reference. Held on its
     * side (gravity along x) it reads turned away from upright; after the reset, upright, and it
     * stays so (the gravity correction does not undo the reset). */
    const float no_turn[3]={0,0,0}, on_side[3]={SDL_STANDARD_GRAVITY,0,0};
    assert(SDL_SendJoystickVirtualSensorData(joystick,SDL_SENSOR_GYRO,1,no_turn,3));
    assert(SDL_SendJoystickVirtualSensorData(joystick,SDL_SENSOR_ACCEL,1,on_side,3));
    pump();
    for (int i=0;i<400;++i) data=read_pad();
    assert(fabsf(data.orientation[3])<0.8f);
    assert(pad_reset_orientation(1)==0);
    data=read_pad();
    assert(fabsf(data.orientation[3])>0.999f);
    for (int i=0;i<50;++i) data=read_pad();
    assert(fabsf(data.orientation[3])>0.999f && fabsf(data.acceleration[0]-1.f)<0.01f);

    /* A Valve controller's shake clips at 2 g: the part beyond gravity is doubled, within 4 g;
     * held still it reads as measured. */
    float gravity[3], still[3]={0,1,0}, shake[3]={0,2,0}, hard[3]={0,4,0};
    accel_boost(still,gravity,2,0);
    assert(still[1]==1);
    accel_boost(shake,gravity,2,0.01f);
    assert(shake[1]>2.9f && shake[1]<3.f);
    accel_boost(hard,gravity,2,0.01f);
    assert(hard[1]==4);

    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    unlink(path);
    puts("PASS: pad ABI, staged left/right clicks, L1 chord, SDL touch coordinates, two trackpads, trackpad haptics, touch IDs, orientation reset, accel gain, overlay capture");
}
