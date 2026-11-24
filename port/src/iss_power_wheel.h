#ifndef ISS_POWER_WHEEL_H
#define ISS_POWER_WHEEL_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { int supported,ready,active,unlocked_mask,error,pending; } IssPowerWheelStatus;
void runtime_power_wheel_attach(unsigned char *image,size_t size);
void runtime_power_wheel_load_begin(void);
void runtime_power_wheel_tick(void);
void runtime_power_wheel_request(int index);
int runtime_power_wheel_gameplay(void);
/* Verified and not paused (the game's pause counter only): unlike runtime_power_wheel_gameplay(),
 * cutscenes count as not paused, so the pause-menu 16:9 presentation does not box them in. */
int runtime_power_wheel_unpaused(void);
void runtime_power_wheel_status(IssPowerWheelStatus *out);
void runtime_power_wheel_visual(int open,int choice);
void runtime_power_wheel_visual_status(int *open,int *choice);
void tlggpu_power_wheel_settings(int *enabled,int keys[4],int *instant);
#ifdef __cplusplus
}
#endif
#endif
