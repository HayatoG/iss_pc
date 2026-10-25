/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Mouse wiggle as the controller's shake (runtime_pad.c): while the graffiti spray can is held
 * (sensor mode, T or G) a quick back-and-forth of the mouse shakes the can, like Y does.
 *
 * Per axis the motion is cut into strokes: counts add up while they keep one direction, and a
 * stroke ends when the direction flips. A stroke of at least `min_stroke` counts that ends within
 * MOUSE_SHAKE_STROKE_S of its start is a swing; MOUSE_SHAKE_SWINGS swings inside
 * MOUSE_SHAKE_WINDOW_S start the shake, and every new swing keeps it going for MOUSE_SHAKE_HOLD_S.
 * Slow aiming (long strokes) and small hand jitter (short strokes) never count. A wheel notch is a
 * swing too (spinning the wheel shakes), unless the wheel is bound to a button.
 * Kept free of SDL and the runtime so tests/test_mouse_shake.c checks the logic alone. */
#ifndef ISS_MOUSE_SHAKE_H
#define ISS_MOUSE_SHAKE_H
#include <math.h>
#include <stdint.h>

#define MOUSE_SHAKE_MIN_STROKE 40.0f /* counts; TLG_MOUSE_SHAKE_COUNTS changes it */
#define MOUSE_SHAKE_STROKE_S 0.25f   /* a swing is quick: one direction for at most this long */
#define MOUSE_SHAKE_WINDOW_S 0.6f
#define MOUSE_SHAKE_SWINGS 3
#define MOUSE_SHAKE_HOLD_S 0.3f
#define MOUSE_SHAKE_HISTORY 8

typedef struct {
    float stroke[2];          /* counts of the current stroke per axis (signed) */
    uint64_t stroke_start[2]; /* us; when the current stroke began */
    uint64_t swings[MOUSE_SHAKE_HISTORY]; /* times of the latest swings, newest at [0] */
    int active;
    uint64_t until;           /* shaking until this time (us) */
} MouseShake;

static inline void mouse_shake_swing(MouseShake *s, uint64_t now) {
    for (int i=MOUSE_SHAKE_HISTORY-1;i>0;--i) s->swings[i]=s->swings[i-1];
    s->swings[0]=now;
    int recent=0;
    for (int i=0;i<MOUSE_SHAKE_HISTORY;++i)
        if (s->swings[i] && now-s->swings[i]<=(uint64_t)(MOUSE_SHAKE_WINDOW_S*1e6f)) ++recent;
    if (recent>=MOUSE_SHAKE_SWINGS || s->active) {
        s->active=1;
        s->until=now+(uint64_t)(MOUSE_SHAKE_HOLD_S*1e6f);
    }
}

/* dx, dy: mouse counts since the last call; wheel: notches since the last call (any direction);
 * now: microseconds. Returns 1 while the can should shake. */
static inline int mouse_shake_update(MouseShake *s, float dx, float dy, int wheel, uint64_t now, float min_stroke) {
    const float d[2]={dx,dy};
    for (int a=0;a<2;++a) {
        if (d[a]==0.0f) continue;
        if (s->stroke[a]!=0.0f && (d[a]>0)!=(s->stroke[a]>0)) {   /* direction flipped: stroke ends */
            const int big=fabsf(s->stroke[a])>=min_stroke;
            const int quick=now-s->stroke_start[a]<=(uint64_t)(MOUSE_SHAKE_STROKE_S*1e6f);
            if (big && quick) mouse_shake_swing(s,now);
            s->stroke[a]=0.0f;
        }
        if (s->stroke[a]==0.0f) s->stroke_start[a]=now;
        s->stroke[a]+=d[a];
    }
    for (int i=0;i<wheel;++i) mouse_shake_swing(s,now);
    if (s->active && now>=s->until) s->active=0;
    return s->active;
}

static inline void mouse_shake_reset(MouseShake *s) {
    *s=(MouseShake){0};
}

#endif
