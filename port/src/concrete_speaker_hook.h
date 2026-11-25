/* Opt-in added Concrete feedback. Uses original local media through Wwise;
 * does not change resources, input, saves or normal PC-speaker audio. */
#ifndef ISS_CONCRETE_SPEAKER_HOOK_H
#define ISS_CONCRETE_SPEAKER_HOOK_H
#include "concrete_speaker_state.h"
#include "power_wheel_gameplay.h"
#include "power_wheel_signatures.h"
static int iss_concrete_speaker_enabled;
static LONG iss_concrete_speaker_busy;
static ConcreteSpeakerState iss_concrete_speaker_state;
static volatile LONG64 iss_concrete_last_cue_ms;
/* Posts the added cue (never from inside the game's PostEvent). Returns the playing id, 0 = failed. */
static uint32_t iss_concrete_speaker_post(uint64_t caller,const char *why,float energy,float amount){
 typedef uint32_t(ABI *PostEvent)(uint32_t,uint64_t,uint32_t,void*,void*,uint32_t,void*,uint32_t);
 /* Hero's audio object 3 is the observed recipient of native Neon completion.
  * Wwise validates object/event registration; failure disables this cue. */
 uint32_t playing=((PostEvent)(image+0x7fb480))(805085824u,3,0,NULL,NULL,0,NULL,0);
 iss_audio_trace_record(0xfff00010u,playing,caller);
 InterlockedExchange64(&iss_concrete_last_cue_ms,(LONG64)GetTickCount64());
 InterlockedExchange(&iss_concrete_absorb_pending,0);
 static unsigned logs;
 if(logs++<12)printf("Concrete speaker cue: playing_id=%u (%s) energy=%.3f add=%.3f\n",playing,why,energy,amount);
 if(!playing){iss_concrete_speaker_enabled=0;puts("Concrete speaker cue: event unavailable; disabled safely");}
 return playing;
}
static void iss_concrete_speaker_add(const uint64_t *r){
 if(!iss_concrete_speaker_enabled || InterlockedCompareExchange(&iss_concrete_speaker_busy,1,0))return;
 uintptr_t hero=(uintptr_t)r[6],live=0;uint32_t native=0,paused=1;float energy=0,maximum=0,amount=0;
 memcpy(&live,image+0x1d0cfe0,8);
 if(hero!=live || hero<0x1000000000ull || hero>=0x10000000000ull)goto done;
 memcpy(&native,image+0xefacf8+0xe4,4);memcpy(&paused,image+PW_PAUSE_COUNTER,4);
 memcpy(&energy,(void*)(hero+0x1a1e8),4);memcpy(&maximum,(void*)(hero+0x1a1f0),4);
 memcpy(&amount,(const unsigned char*)r-128,4); /* Saved incoming XMM0. */
 /* Unlimited energy mod (user/mods/iss_infinite_energy.xml): its first line NOPs the energy store at
  * 0x20c7f9 (10 bytes), keeping energy full, so a Concrete refill never happens below full (2026-11-06). */
 static int unlimited=-1;
 if(unlimited<0){unlimited=1;for(int i=0;i<10;i++)if(image[0x20c7f9+i]!=0x90)unlimited=0;
  if(unlimited)puts("Concrete speaker cue: Unlimited energy mod found; refills at full energy also play the cue");}
 ConcreteSpeakerSample sample={1,(int)paused,(int)native,hero,r[7]-(uintptr_t)image,energy,maximum,amount,GetTickCount64(),unlimited};
 /* The game just played its Concrete-absorb sound: this refill burst is a Concrete absorb whatever power
  * is still active (the game switches to Concrete partway through), so count it as Concrete. */
 {const uint64_t absorbed=(uint64_t)InterlockedCompareExchange64(&iss_concrete_absorb_ms,0,0);
  if(absorbed && sample.now>=absorbed && sample.now-absorbed<2500)sample.native=4;}
 /* Diagnostic (2026-11-06): what the cue sees for Concrete refills (the cue did not play over Bluetooth). */
 {static unsigned seen;if(native==4 && seen++<30)printf("Concrete speaker cue: refill seen caller=%#llx energy=%.2f/%.2f add=%.3f paused=%u\n",
   (unsigned long long)sample.caller,energy,maximum,amount,paused);}
 if(concrete_speaker_begin(&iss_concrete_speaker_state,&sample)){
  /* Once per absorb: the game-thread path below may already have played it for this absorb. */
  const uint64_t absorbed=(uint64_t)InterlockedCompareExchange64(&iss_concrete_absorb_ms,0,0);
  const uint64_t last=(uint64_t)InterlockedCompareExchange64(&iss_concrete_last_cue_ms,0,0);
  if(!(absorbed && last>=absorbed && sample.now-absorbed<2500))
   iss_concrete_speaker_post(sample.caller,"refill",energy,amount);
 }
done:
 InterlockedExchange(&iss_concrete_speaker_busy,0);
}
/* Game camera-thread tick (runtime_karma_tick). Absorbing Concrete while Concrete is already the power
 * and energy is full makes no refill call at all, so the refill path above never runs; the game's own
 * absorb sound (ISS_CONCRETE_ABSORB_EVENT) still does. 0.6 s after it, if no cue has played for that
 * absorb, play it here: every Concrete absorb gets the cue, once (2026-11-06). */
extern void (*volatile runtime_karma_extra_tick)(void);
static void iss_concrete_speaker_tick(void){
 if(!iss_concrete_speaker_enabled || !InterlockedCompareExchange(&iss_concrete_absorb_pending,0,0))return;
 const uint64_t now=GetTickCount64(),absorbed=(uint64_t)InterlockedCompareExchange64(&iss_concrete_absorb_ms,0,0);
 if(now<absorbed+600)return; /* the refill path normally plays it first, ~0.4 s after the absorb sound */
 if(!InterlockedExchange(&iss_concrete_absorb_pending,0))return;
 const uint64_t last=(uint64_t)InterlockedCompareExchange64(&iss_concrete_last_cue_ms,0,0);
 if(last>=absorbed || now-absorbed>3000)return; /* already played for this absorb, or too late */
 uint32_t paused=1;memcpy(&paused,image+PW_PAUSE_COUNTER,4);if(paused)return;
 if(InterlockedCompareExchange(&iss_concrete_speaker_busy,1,0))return;
 iss_concrete_speaker_post(0,"absorb",0.f,0.f);
 InterlockedExchange(&iss_concrete_speaker_busy,0);
}
static void iss_concrete_speaker_enable(uint64_t size){
 const char *opt=getenv("ISS_CONCRETE_SPEAKER_CUE");if(!opt || strcmp(opt,"1"))return;
 static const unsigned char post[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x38,0x44,0x89,0xcb,0x4c,0x89,0x45,0xb0,0x49,0x89,0xcf,0x41,0x89,0xd4,0x49,0x89};
 if(size<PW_PAUSE_COUNTER+4 || !pw_pause_signature(image,size) || !pw_signatures(image,size) ||
    memcmp(image+0x7fb480,post,sizeof(post))){puts("Concrete speaker cue: incompatible executable; disabled");return;}
 iss_concrete_speaker_enabled=1;
 runtime_karma_extra_tick=iss_concrete_speaker_tick;
 puts("Concrete speaker cue: guarded script-refill feedback enabled (added cue, not original game behavior)");
}
#endif
