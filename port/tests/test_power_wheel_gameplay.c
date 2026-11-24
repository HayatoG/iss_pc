/* Exercise the real request/visual gate while camera ticks are stopped by menus. */
#include "../src/runtime_power_wheel.c"
#include <assert.h>
int tlggpu_overlay_captures_input(void){return 0;}
static int camera_live=-1;
int runtime_mouse_aim_camera_live(void){return camera_live;}
void tlggpu_request_screenshot(void){}
int main(void){
    /* Supported code can be installed before guest writable pages exist. */
    pw_image=(unsigned char*)0x800000000ull;
    atomic_store(&pw_supported,1);atomic_store(&pw_ready,0);
    assert(!runtime_power_wheel_gameplay());
    pw_image=calloc(1,PW_PAUSE_COUNTER+4);assert(pw_image);
    atomic_store(&pw_supported,1);atomic_store(&pw_ready,1);
    runtime_power_wheel_request(PW_VIDEO);assert(atomic_load(&pw_requested)==PW_VIDEO+1);
    runtime_power_wheel_visual(1,PW_VIDEO);
    int open,choice;runtime_power_wheel_visual_status(&open,&choice);assert(open);
    uint32_t paused=1;memcpy(pw_image+PW_PAUSE_COUNTER,&paused,4);
    runtime_power_wheel_visual_status(&open,&choice);
    assert(!open&&!atomic_load(&pw_requested));
    runtime_power_wheel_request(PW_SMOKE);assert(!atomic_load(&pw_requested));
    runtime_power_wheel_visual(1,PW_SMOKE);assert(!atomic_load(&pw_visual_open));
    paused=2;memcpy(pw_image+PW_PAUSE_COUNTER,&paused,4);
    assert(!runtime_power_wheel_gameplay());
    paused=0;memcpy(pw_image+PW_PAUSE_COUNTER,&paused,4);
    runtime_power_wheel_visual_status(&open,&choice);assert(!open);
    assert(!atomic_load(&pw_requested));
    runtime_power_wheel_request(PW_NEON);assert(atomic_load(&pw_requested)==PW_NEON+1);
    /* Cutscene: player camera stopped hides the wheel and cancels requests; running again restores. */
    camera_live=1;runtime_power_wheel_visual(1,PW_NEON);
    runtime_power_wheel_visual_status(&open,&choice);assert(open);
    camera_live=0;runtime_power_wheel_visual_status(&open,&choice);
    assert(!open&&!atomic_load(&pw_requested));
    runtime_power_wheel_request(PW_SMOKE);assert(!atomic_load(&pw_requested));
    camera_live=1;assert(runtime_power_wheel_gameplay());
    runtime_power_wheel_request(PW_NEON);assert(atomic_load(&pw_requested)==PW_NEON+1);
    camera_live=-1;
    runtime_power_wheel_load_begin();assert(!atomic_load(&pw_requested));
    atomic_store(&pw_supported,0);free(pw_image);pw_image=NULL;
    assert(!runtime_power_wheel_gameplay());
    puts("Gameplay gate: paused visuals/requests blocked, pending canceled, nested pause, resume and load passed.");
}
