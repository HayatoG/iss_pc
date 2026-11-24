/* TLG_PAD_MAP / TLG_KEY_MAP parsing (src/runtime_pad.c). */
#define _GNU_SOURCE
#include <assert.h>
#include "../src/runtime_pad.c"
#include "test_pad_audio_stubs.h"
// runtime_mouse_aim.c (the native aim hook) needs the guest; pad tests run without it.
int runtime_mouse_aim_installed(void) { return 0; }
void runtime_power_wheel_status(IssPowerWheelStatus *out){memset(out,0,sizeof(*out));}
void runtime_power_wheel_request(int index){(void)index;}
void runtime_power_wheel_visual(int open,int choice){(void)open;(void)choice;}
void tlggpu_power_wheel_settings(int *enabled,int keys[4],int *instant){*enabled=0;*instant=0;memset(keys,0,4*sizeof(*keys));}
int runtime_power_wheel_gameplay(void){return 0;}
float tlggpu_sensor_aim_scale(void){return 1.0f;}
void tlggpu_graffiti_guide(int force){(void)force;}
int runtime_mouse_aim_motion(float dx, float dy, float sensitivity, int invert_y, int active) { (void)dx; (void)dy; (void)sensitivity; (void)invert_y; (void)active; return 0; }

/* The GPU library (not linked here): the mouse and the input settings. */
void tlggpu_mouse_take(TlgGpuMouse *mouse) { memset(mouse,0,sizeof(*mouse)); }
void tlggpu_mouse_pad_hud(int a, float x, float y, int t, int c) { (void)a; (void)x; (void)y; (void)t; (void)c; }
void tlggpu_input_settings(TlgGpuInputSettings *settings) { memset(settings,0,sizeof(*settings)); }
uint64_t tlggpu_total_draws(void) { return 0; }
void tlggpu_frame_counters(TlgGpuFrameCounters *out) { memset(out,0,sizeof(*out)); }
int tlggpu_overlay_captures_input(void) { return 0; }
void tlggpu_overlay_notice(const char *text, float seconds) { (void)text; (void)seconds; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

/* PS4 buttons a host source presses in `map`. */
static uint32_t pressed_by(const ButtonMap *map, int source) {
    uint32_t ps=0;
    for (size_t i=0;i<map->count;++i) if (map->entries[i].source==source) ps|=map->entries[i].ps;
    return ps;
}

int main(void) {
    ButtonMap keys=key_map;
    assert(apply_button_map(&keys,NULL,true)==0 && apply_button_map(&keys,"",true)==0);
    assert(keys.count==key_map.count);

    /* Swap: Q presses Cross, Space presses Triangle; Space no longer presses Cross. */
    assert(apply_button_map(&keys," cross = Q , triangle=Space",true)==2);
    assert(pressed_by(&keys,SDL_SCANCODE_Q)==BTN_CROSS);
    assert(pressed_by(&keys,SDL_SCANCODE_SPACE)==BTN_TRIANGLE);
    assert(pressed_by(&keys,SDL_SCANCODE_E)==BTN_SQUARE);

    /* Names with spaces, any case; the last entry for a button wins. */
    ButtonMap again=key_map;
    assert(apply_button_map(&again,"CIRCLE=left shift,circle=Left Ctrl",true)==2);
    assert(pressed_by(&again,SDL_SCANCODE_LCTRL)==BTN_CIRCLE);
    assert(pressed_by(&again,SDL_SCANCODE_LSHIFT)==0);

    /* Bad entries are skipped and leave the defaults alone. */
    ButtonMap bad=key_map;
    assert(apply_button_map(&bad,"bogus=Q,cross=NotAKey,circle,=Q,square=,,",true)==0);
    assert(bad.count==key_map.count && pressed_by(&bad,SDL_SCANCODE_SPACE)==BTN_CROSS);

    /* Gamepad: Xbox layout swap; 'touchpad' is the left click, Back stays the right click and
     * the real touchpad click keeps its fingers. */
    ButtonMap pad=pad_map;
    assert(apply_button_map(&pad,"cross=b,circle=a,touchpad=misc1",false)==3);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_EAST)==BTN_CROSS);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_SOUTH)==BTN_CIRCLE);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_MISC1)==CLICK_LEFT);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_BACK)==CLICK_RIGHT);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_TOUCHPAD)==BTN_TOUCHPAD);
    assert(apply_button_map(&pad,"touchpad_right=guide,touchpad_left=paddle1",false)==2);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_BACK)==0 && pressed_by(&pad,SDL_GAMEPAD_BUTTON_GUIDE)==CLICK_RIGHT);
    assert(pressed_by(&pad,SDL_GAMEPAD_BUTTON_MISC1)==0 && pressed_by(&pad,SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1)==CLICK_LEFT);
    assert(apply_button_map(&pad,"cross=Space",false)==0); /* a key name is no gamepad button */
    /* Keyboard: each side has a key of its own. */
    assert(pressed_by(&key_map,SDL_SCANCODE_TAB)==CLICK_LEFT && pressed_by(&key_map,SDL_SCANCODE_BACKSPACE)==CLICK_RIGHT);
    ButtonMap sides=key_map;
    assert(apply_button_map(&sides,"touchpad_right=Delete",true)==1);
    assert(pressed_by(&sides,SDL_SCANCODE_BACKSPACE)==0 && pressed_by(&sides,SDL_SCANCODE_DELETE)==CLICK_RIGHT);
    /* Stick directions (2026-11-06): WASD / arrows by default, rebindable like the buttons. */
    assert(pressed_by(&key_map,SDL_SCANCODE_W)==LS_UP && pressed_by(&key_map,SDL_SCANCODE_D)==LS_RIGHT);
    assert(pressed_by(&key_map,SDL_SCANCODE_UP)==RS_UP && pressed_by(&key_map,SDL_SCANCODE_LEFT)==RS_LEFT);
    ButtonMap moved=key_map;
    assert(apply_button_map(&moved,"ls_up=Up,rs_up=I",true)==2);
    assert(pressed_by(&moved,SDL_SCANCODE_UP)==LS_UP && pressed_by(&moved,SDL_SCANCODE_W)==0);
    assert(pressed_by(&moved,SDL_SCANCODE_I)==(RS_UP|BTN_UP));

    /* The in-game bindings window: live apply over the base map, the lookup, reset. */
    char text[128];
    assert(runtime_pad_key_binding("cross",0,text,sizeof(text))==1 && !strcmp(text,"Space"));
    assert(runtime_pad_key_binding("ls_left",1,text,sizeof(text))==1 && !strcmp(text,"A"));
    assert(runtime_pad_key_binding("r2",0,text,sizeof(text))==2 && strstr(text,"F") && strstr(text,"Mouse Left"));
    assert(runtime_pad_key_binding("nonsense",0,text,sizeof(text))==-1);
    assert(runtime_pad_set_key_binds("cross=Mouse X1,ls_up=none")==2);
    assert(runtime_pad_key_binding("cross",0,text,sizeof(text))==1 && !strcmp(text,"Mouse X1"));
    assert(runtime_pad_key_binding("cross",1,text,sizeof(text))==1 && !strcmp(text,"Space")); /* default kept */
    assert(runtime_pad_key_binding("ls_up",0,text,sizeof(text))==0 && !*text);
    assert(runtime_pad_set_key_binds("")==0);
    assert(runtime_pad_key_binding("cross",0,text,sizeof(text))==1 && !strcmp(text,"Space"));
    assert(runtime_pad_key_binding("ls_up",0,text,sizeof(text))==1 && !strcmp(text,"W"));
    puts("PASS: pad and key maps, stick directions, in-game bindings");
}
