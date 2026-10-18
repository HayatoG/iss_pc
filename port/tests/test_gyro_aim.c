/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <float.h>
#include <stdio.h>
#include "../src/gyro_aim.h"

static float filtered(float step) {
    GyroAim s={0};
    const GyroAimParams p={1,0.05f,0,0,0};
    const int n=(int)lroundf(0.2f/step);
    for (int i=0;i<n;++i) gyro_aim_update(&s,&p,0,GYRO_AIM_FULL_SPEED/2,step,1);
    return gyro_aim_axis(&s,&p,0);
}

int main(void) {
    GyroAim s={0};
    GyroAimParams p={1,0,0,0,0};
    gyro_aim_update(&s,&p,0,GYRO_AIM_FULL_SPEED/2,0.01f,1);
    assert(fabsf(gyro_aim_axis(&s,&p,0)+0.5f)<1e-6f); /* yaw left */
    assert(gyro_aim_axis(&s,&p,1)==0);
    gyro_aim_update(&s,&p,GYRO_AIM_FULL_SPEED,0,0.01f,1);
    assert(gyro_aim_axis(&s,&p,0)==0 && gyro_aim_axis(&s,&p,1)==-1); /* pitch up */
    p.invert_y=1;
    gyro_aim_update(&s,&p,GYRO_AIM_FULL_SPEED,0,0.01f,1);
    assert(gyro_aim_axis(&s,&p,1)==1);
    p.invert_y=0;
    p.sensitivity=2;
    gyro_aim_update(&s,&p,0,-GYRO_AIM_FULL_SPEED/2,0.01f,1);
    assert(gyro_aim_axis(&s,&p,0)==1);
    gyro_aim_update(&s,&p,0,-1000,0.01f,1);
    assert(gyro_aim_axis(&s,&p,0)==1); /* saturation */
    assert(gyro_aim_add(200,1)==255 && gyro_aim_add(55,-1)==0);
    assert(gyro_aim_add(200,-0.5f)==136); /* adds to, does not replace, physical stick */
    assert(gyro_aim_add(192,0)==192 && gyro_aim_add(128,NAN)==128);

    assert(fabsf(filtered(0.01f)-filtered(0.001f))<1e-5f); /* same smoothing at 100/1000 Hz */
    gyro_aim_reset(&s);
    p=(GyroAimParams){1,0.1f,0.01f,0.1f,0};
    gyro_aim_update(&s,&p,0,1,0.01f,1);
    assert(gyro_aim_axis(&s,&p,0)<-0.1f);
    gyro_aim_update(&s,&p,0.002f,-0.003f,0.01f,1);
    assert(s.out[0]==0 && s.out[1]==0); /* rest noise does not drift or leave a filter tail */
    gyro_aim_update(&s,&p,0,1,0.01f,1);
    gyro_aim_update(&s,&p,0,1,0.01f,0);
    assert(s.out[0]==0 && s.out[1]==0); /* release/disabled/focus/graffiti -> immediate neutral */
    const float bad[][3]={{NAN,1,.01f},{1,INFINITY,.01f},{1,1,NAN},{1,1,0},{1,1,-1},{1,1,5},{FLT_MAX,FLT_MAX,.01f}};
    for (unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        gyro_aim_update(&s,&p,bad[i][0],bad[i][1],bad[i][2],1);
        assert(s.out[0]==0 && s.out[1]==0);
    }
    p=(GyroAimParams){NAN,NAN,NAN,NAN,0};
    gyro_aim_update(&s,&p,1,1,.01f,1);
    assert(isfinite(gyro_aim_axis(&s,&p,0)) && isfinite(gyro_aim_axis(&s,&p,1)));
    puts("PASS: gyro aim signs, sensitivity, noise, smoothing, release, stick mixing and invalid samples");
    return 0;
}
