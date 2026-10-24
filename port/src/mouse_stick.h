/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Mouse motion as a right stick (runtime_pad.c): the game only knows a stick, so the mouse speed
 * becomes a deflection. Per axis, every pad sample:
 *   1. counts and time add up until a window of MOUSE_STICK_WINDOW_S (mouse reports arrive at
 *      125-1000 Hz, the pad may be sampled faster); the speed of the window is the target;
 *   2. target = speed * sensitivity / MOUSE_STICK_FULL_SPEED (times vertical on y), times
 *      (1 + accel * |target|), clamped to [-1, 1];
 *   3. smoothing: the deflection moves toward the target with an exponential filter, the same
 *      per second at any sample rate (0 none, 0.9 heavy);
 *   4. once the mouse stops the deflection decays to exactly 0 (below MOUSE_STICK_REST): no
 *      drift; a moving mouse is lifted past the game's dead zone by `deadzone` (anti-dead zone).
 * Kept free of SDL and the runtime so tests/test_mouse_stick.c checks the math alone. */
#ifndef ISS_MOUSE_STICK_H
#define ISS_MOUSE_STICK_H
#include <math.h>
#include <stdint.h>

#define MOUSE_STICK_FULL_SPEED 2400.0f /* counts/s for a full deflection at sensitivity 1 */
#define MOUSE_STICK_WINDOW_S 0.016f
#define MOUSE_STICK_REST 0.01f

typedef struct {
    float sensitivity; /* 0.05 .. 10 */
    float smoothing;   /* 0 .. 0.95 */
    float accel;       /* 0 .. 2 */
    float deadzone;    /* 0 .. 0.5 */
    int invert_y;
    float vertical;    /* 0.25 .. 4, y's sensitivity over x's (the game pitches slower); 0 = 1 */
} MouseStickParams;

typedef struct {
    float acc[2], acc_t; /* counts and seconds of the current window */
    float target[2];
    float out[2];        /* filtered deflection, -1 .. 1, before the anti-dead zone */
} MouseStick;

static inline float mouse_stick_clamp(float v, float lo, float hi) { return v<lo ? lo : v>hi ? hi : v; }

/* dx, dy: mouse counts since the last call; dt: seconds since it. */
static inline void mouse_stick_update(MouseStick *s, const MouseStickParams *p, float dx, float dy, float dt) {
    if (dt>0.1f) { dx*=0.1f/dt; dy*=0.1f/dt; } /* a long gap (loading): keep the speed */
    dt=mouse_stick_clamp(dt,0.0005f,0.1f);
    s->acc[0]+=dx; s->acc[1]+=p->invert_y ? -dy : dy;
    s->acc_t+=dt;
    if (s->acc_t>=MOUSE_STICK_WINDOW_S-1e-5f) {
        const float sens=mouse_stick_clamp(p->sensitivity,0.05f,10.0f), accel=mouse_stick_clamp(p->accel,0.0f,2.0f);
        const float vertical=p->vertical>0.0f ? mouse_stick_clamp(p->vertical,0.25f,4.0f) : 1.0f;
        for (int i=0;i<2;++i) {
            float t=s->acc[i]/s->acc_t*sens*(i ? vertical : 1.0f)/MOUSE_STICK_FULL_SPEED;
            t*=1.0f+accel*fabsf(t);
            s->target[i]=mouse_stick_clamp(t,-1.0f,1.0f);
            s->acc[i]=0;
        }
        s->acc_t=0;
    }
    const float smooth=mouse_stick_clamp(p->smoothing,0.0f,0.95f);
    const float k=smooth<=0.0f ? 1.0f : 1.0f-powf(smooth,dt*60.0f);
    for (int i=0;i<2;++i) {
        s->out[i]+=(s->target[i]-s->out[i])*k;
        if (s->target[i]==0.0f && fabsf(s->out[i])<MOUSE_STICK_REST) s->out[i]=0.0f;
    }
}

/* The deflection of one axis after the anti-dead zone, -1 .. 1 (0 at rest). */
static inline float mouse_stick_axis(const MouseStick *s, const MouseStickParams *p, int axis) {
    const float v=s->out[axis], a=fabsf(v);
    if (a<MOUSE_STICK_REST) return 0.0f;
    const float dz=mouse_stick_clamp(p->deadzone,0.0f,0.5f);
    const float lifted=mouse_stick_clamp(dz+(1.0f-dz)*a,0.0f,1.0f);
    return v<0 ? -lifted : lifted;
}

/* Adds a deflection to a PS4 stick byte (128 centre, 0 left/up, 255 right/down). */
static inline uint8_t mouse_stick_add(uint8_t base, float deflection) {
    if (deflection==0.0f) return base;
    const float v=(float)base+deflection*(deflection<0 ? 128.0f : 127.0f);
    return (uint8_t)mouse_stick_clamp(roundf(v),0.0f,255.0f);
}

#endif
