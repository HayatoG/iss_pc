/* Mouse wiggle to controller shake (src/mouse_shake.h): quick swings shake, aiming and jitter do not. */
#include <assert.h>
#include <stdio.h>
#include "../src/mouse_shake.h"

#define MS 1000ull

/* Moves `counts` along x over `ms`, in 4 ms reports; returns the last shake state. */
static int stroke(MouseShake *s, uint64_t *now, float counts, int ms, int *ever) {
    int on=0;
    const int steps=ms/4 ? ms/4 : 1;
    for (int i=0;i<steps;++i) {
        *now+=4*MS;
        on=mouse_shake_update(s,counts/(float)steps,0,0,*now,MOUSE_SHAKE_MIN_STROKE);
        if (on && ever) *ever=1;
    }
    return on;
}

static int idle(MouseShake *s, uint64_t *now, int ms) {
    int on=0;
    for (int i=0;i<ms/4;++i) { *now+=4*MS; on=mouse_shake_update(s,0,0,0,*now,MOUSE_SHAKE_MIN_STROKE); }
    return on;
}

int main(void) {
    MouseShake s={0};
    uint64_t now=1000*MS;
    int ever=0;

    /* Aiming: slow sweeps back and forth (big strokes, 600 ms each) never shake. */
    for (int i=0;i<8;++i) stroke(&s,&now,(i&1) ? -300.f : 300.f,600,&ever);
    assert(!ever);

    /* Hand jitter: fast but tiny strokes (8 counts) never shake. */
    mouse_shake_reset(&s);
    for (int i=0;i<40;++i) stroke(&s,&now,(i&1) ? -8.f : 8.f,20,&ever);
    assert(!ever);

    /* A wiggle: 80-count strokes every 100 ms. A swing is counted when the direction flips, so
     * the third flip (the start of the fourth stroke) starts the shake, which lasts while it goes on. */
    mouse_shake_reset(&s);
    assert(!stroke(&s,&now,80,100,NULL));
    assert(!stroke(&s,&now,-80,100,NULL));   /* flip 1 */
    assert(!stroke(&s,&now,80,100,NULL));    /* flip 2 */
    assert(stroke(&s,&now,-80,100,NULL));    /* flip 3: shaking */
    for (int i=0;i<10;++i) assert(stroke(&s,&now,(i&1) ? -80.f : 80.f,100,NULL));

    /* Stops within the hold time after the wiggle ends. */
    assert(!idle(&s,&now,(int)(MOUSE_SHAKE_HOLD_S*1000)+20));

    /* Vertical wiggle works too. */
    mouse_shake_reset(&s);
    int on=0;
    for (int i=0;i<6;++i)
        for (int j=0;j<25;++j) { now+=4*MS; on=mouse_shake_update(&s,0,(i&1) ? -4.f : 4.f,0,now,MOUSE_SHAKE_MIN_STROKE); }
    assert(on);

    /* The wheel: spinning it (a notch per report) shakes; one notch alone does not. */
    mouse_shake_reset(&s);
    now+=4*MS;
    assert(!mouse_shake_update(&s,0,0,1,now,MOUSE_SHAKE_MIN_STROKE));
    now+=500*MS*2;                                  /* that notch drops out of the window */
    assert(!mouse_shake_update(&s,0,0,1,now,MOUSE_SHAKE_MIN_STROKE));
    now+=30*MS; assert(!mouse_shake_update(&s,0,0,1,now,MOUSE_SHAKE_MIN_STROKE));
    now+=30*MS; assert(mouse_shake_update(&s,0,0,1,now,MOUSE_SHAKE_MIN_STROKE));

    /* A higher threshold (TLG_MOUSE_SHAKE_COUNTS) ignores the same wiggle. */
    mouse_shake_reset(&s);
    on=0;
    for (int i=0;i<10;++i)
        for (int j=0;j<25;++j) { now+=4*MS; on|=mouse_shake_update(&s,(i&1) ? -3.2f : 3.2f,0,0,now,200.0f); }
    assert(!on);

    puts("mouse shake tests passed");
    return 0;
}
