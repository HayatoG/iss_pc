/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Optional gyro aiming as a right stick. SDL reports angular velocity in rad/s;
 * this is a velocity mapping, not accumulated angles or mouse events. The game's
 * stick response/turn-speed limit still applies. Pure math for deterministic tests. */
#ifndef ISS_GYRO_AIM_H
#define ISS_GYRO_AIM_H
#include <math.h>
#include <stdint.h>
#include <string.h>

#define GYRO_AIM_FULL_SPEED 3.14159265358979323846f /* 180 degrees/s at sensitivity 1 */

typedef struct {
    float sensitivity; /* 0.1 .. 5 */
    float smoothing;   /* time constant, 0 .. 0.2 seconds */
    float noise;       /* radial noise threshold, 0 .. 0.1 rad/s */
    float deadzone;    /* anti-dead zone, 0 .. 0.5 stick units */
    int invert_y;
} GyroAimParams;

typedef struct { float out[2]; } GyroAim;

static inline void gyro_aim_reset(GyroAim *s) { memset(s,0,sizeof(*s)); }
static inline float gyro_aim_clamp(float v, float lo, float hi, float fallback) {
    return !isfinite(v) ? fallback : v<lo ? lo : v>hi ? hi : v;
}

/* Positive SDL yaw turns left; positive pitch looks up. Both are negative stick axes.
 * Inactive, invalid samples and gaps after a pause release immediately, without a tail. */
static inline void gyro_aim_update(GyroAim *s, const GyroAimParams *p,
                                   float pitch, float yaw, float dt, int active) {
    if (!active || !isfinite(pitch) || !isfinite(yaw) || !isfinite(dt) || dt<=0 || dt>0.25f) {
        gyro_aim_reset(s);
        return;
    }
    const float noise=gyro_aim_clamp(p->noise,0,0.1f,0.01f);
    const float magnitude=hypotf(pitch,yaw);
    if (!isfinite(magnitude) || magnitude<=noise) { gyro_aim_reset(s); return; }
    const float gain=(magnitude-noise)/magnitude *
                     gyro_aim_clamp(p->sensitivity,0.1f,5,2.5f)/GYRO_AIM_FULL_SPEED;
    const float target[2]={-yaw*gain,(p->invert_y ? pitch : -pitch)*gain};
    const float tau=gyro_aim_clamp(p->smoothing,0,0.2f,0.025f);
    const float blend=tau>0 ? -expm1f(-dt/tau) : 1;
    for (int i=0;i<2;++i) {
        const float v=gyro_aim_clamp(target[i],-1,1,0);
        s->out[i]+=(v-s->out[i])*blend;
    }
}

static inline float gyro_aim_axis(const GyroAim *s, const GyroAimParams *p, int axis) {
    const float v=s->out[axis], a=fabsf(v);
    if (a<0.0001f) return 0;
    const float zone=gyro_aim_clamp(p->deadzone,0,0.5f,0.1f);
    const float lifted=fminf(zone+(1-zone)*a,1);
    return v<0 ? -lifted : lifted;
}

static inline uint8_t gyro_aim_add(uint8_t stick, float deflection) {
    if (!isfinite(deflection) || deflection==0) return stick;
    const float value=roundf(stick+deflection*(deflection<0 ? 128 : 127));
    return (uint8_t)fminf(fmaxf(value,0),255);
}
#endif
