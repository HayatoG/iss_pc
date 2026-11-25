#ifndef ISS_CONCRETE_SPEAKER_STATE_H
#define ISS_CONCRETE_SPEAKER_STATE_H
#include <stdint.h>
#include <math.h>
typedef struct { uintptr_t hero; uint64_t last; int active; } ConcreteSpeakerState;
typedef struct {
 int compatible,paused,native;
 uintptr_t hero,caller;
 float energy,maximum,amount;
 uint64_t now;
 int full_ok; /* Unlimited energy mod on: energy always reads full, so a refill at full still counts */
} ConcreteSpeakerSample;
/* Observe only the measured script-driven energy-add path. No resource writes. */
static inline int concrete_speaker_begin(ConcreteSpeakerState *s,const ConcreteSpeakerSample *v){
 if(!v->compatible || v->paused || v->native!=4 || !v->hero ||
    !isfinite(v->energy)||!isfinite(v->maximum)||
    v->maximum<=0 || v->maximum>100000 || v->energy<0){s->active=0;return 0;}
 /* With the Unlimited energy mod the absorb script's add is (target - current) with energy held at or
  * above full, so the measured refill calls arrive with a small negative amount (about -0.31 per tick,
  * logged 2026-11-06); any non-zero amount from the refill caller then counts. */
 const float size=v->full_ok ? fabsf(v->amount) : v->amount;
 if(v->caller!=0x22e749 || !isfinite(v->amount) || (v->energy>=v->maximum-.0001f && !v->full_ok) ||
    size<=0 || size>v->maximum*2)return 0;
 int start=!s->active || s->hero!=v->hero || v->now<s->last || v->now-s->last>250;
 s->hero=v->hero;s->last=v->now;s->active=1;return start;
}
#endif
