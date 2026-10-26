/* System-library functions first needed by The Last Guardian (CUSA03627 v1.03).
 *
 * Event flags are a complete implementation. NGS2 is a silent placeholder that lets the game's
 * sndx engine initialize and run (AudioOut still paces the audio thread); TLG_NGS2=native links
 * the firmware's NGS2 instead (see analysis/03_audio.md). Audio3d lives in runtime_gr2_audio3d.c.
 * Everything else is a small contract the game needs at boot: kernel queries, time-zone
 * conversion, PlayGo dialog, LoadExec.
 */
#define _GNU_SOURCE
#include "runtime.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define KERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define KERR_ESRCH KERR(3)
#define KERR_EPERM KERR(1)
#define KERR_EBUSY KERR(16)
#define KERR_EINVAL KERR(22)
#define KERR_ENOMEM KERR(12)
#define KERR_ETIMEDOUT KERR(60)
#define KERR_ECANCELED KERR(85)

static void once(const char *what) {
    /* First call of a placeholder is logged; the list is short and fixed. */
    static const char *seen[64];
    static int count;
    static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    for (int i=0;i<count;++i) if (seen[i]==what) { pthread_mutex_unlock(&lock); return; }
    if (count<64) seen[count++]=what;
    pthread_mutex_unlock(&lock);
    printf("Runtime: %s (placeholder)\n",what);
}

/* ---- Event flags ---- */
#define EF_MAGIC UINT32_C(0x45564654)
#define EF_ATTR_MULTI 0x20
#define EF_WAIT_AND 1
#define EF_WAIT_OR 2
#define EF_CLEAR_ALL 0x10
#define EF_CLEAR_PAT 0x20
/* A blocked waiter, enrolled in arrival order (FIFO) for the duration of its wait. */
typedef struct EfWaiter {
    uint64_t pattern;
    uint32_t mode;
    int done;          /* TLG_EF_HANDOFF: a set satisfied (and consumed for) this waiter */
    uint64_t result;   /* the bits that set saw, before its clear */
#ifdef _WIN32
    CONDITION_VARIABLE cv;   /* TLG_EF_SRW */
#endif
    struct EfWaiter *next;
} EfWaiter;
typedef struct {
    uint32_t magic, attr;
    uint64_t bits;
    int waiters, deleted, cancel_generation;
    pthread_mutex_t lock;
    pthread_cond_t cond;
#ifdef _WIN32
    SRWLOCK srw;
#endif
    EfWaiter *first;
    char name[32];
} EventFlag;
/* TLG_EF_WAKE_WAITERS=1: a set wakes only the waiters whose pattern the new bits satisfy (with the
 * shared condition variable: it broadcasts only when at least one is satisfied) instead of every
 * waiter. A woken waiter still re-checks and consumes under the lock, as before, so the outcome is
 * the same; only the wakeups that would find their pattern unsatisfied are skipped.
 * TLG_EF_SRW=1 (Windows): the flag's lock is an SRWLOCK and each waiter sleeps on its own
 * CONDITION_VARIABLE instead of the winpthreads mutex and condition variable; deadlines then use
 * the monotonic clock. Both are read once. */
static int ef_toggle(int *cache,const char *name) {
    int v=__atomic_load_n(cache,__ATOMIC_RELAXED);
    if (v<0) {
        const char *e=getenv(name);
        v=e && e[0]=='1';
        __atomic_store_n(cache,v,__ATOMIC_RELAXED);
    }
    return v;
}
static int ef_selective(void) { static int v=-1; return ef_toggle(&v,"TLG_EF_WAKE_WAITERS"); }
/* TLG_EF_HANDOFF (default on, =0 disables): a set hands the bits to each waiter it satisfies, in
 * FIFO order, and applies that waiter's clear mode at once, as the PS4 kernel does. Without it a
 * waiter re-tests the bits only once it has retaken the lock, so a set followed at once by a clear
 * (or another waiter's consume) loses the wakeup. Read once. */
static int ef_handoff(void) {
    static int v=-1;
    int c=__atomic_load_n(&v,__ATOMIC_RELAXED);
    if (c<0) { const char *e=getenv("TLG_EF_HANDOFF"); c=!(e && e[0]=='0'); __atomic_store_n(&v,c,__ATOMIC_RELAXED); }
    return c;
}
#ifdef _WIN32
static int ef_srw(void) { static int v=-1; return ef_toggle(&v,"TLG_EF_SRW"); }
#else
static int ef_srw(void) { return 0; }
#endif
static size_t ef_sets, ef_blocking, ef_wakeups, ef_skipped;   /* runtime_counters */
#define EF_COUNT(c) do { if (runtime_counters) __atomic_fetch_add(&(c),1,__ATOMIC_RELAXED); } while (0)
static void ef_lock(EventFlag *ef) {
#ifdef _WIN32
    if (ef_srw()) { AcquireSRWLockExclusive(&ef->srw); return; }
#endif
    pthread_mutex_lock(&ef->lock);
}
static void ef_unlock(EventFlag *ef) {
#ifdef _WIN32
    if (ef_srw()) { ReleaseSRWLockExclusive(&ef->srw); return; }
#endif
    pthread_mutex_unlock(&ef->lock);
}
static int ef_match(uint64_t bits,uint64_t pattern,uint32_t mode) {
    return (mode & EF_WAIT_AND) ? (bits & pattern)==pattern : (bits & pattern)!=0;
}
/* Wakes the waiters: all of them, or with `satisfied` only those whose pattern the bits match. */
static void ef_wake(EventFlag *ef,int satisfied) {
#ifdef _WIN32
    if (ef_srw()) {
        for (EfWaiter *w=ef->first;w;w=w->next) {
            if (satisfied && !ef_match(ef->bits,w->pattern,w->mode)) { EF_COUNT(ef_skipped); continue; }
            WakeConditionVariable(&w->cv); EF_COUNT(ef_wakeups);
        }
        return;
    }
#endif
    if (satisfied) {
        int any=0;
        for (EfWaiter *w=ef->first;w && !any;w=w->next) any=ef_match(ef->bits,w->pattern,w->mode);
        if (!any) { if (ef->first) EF_COUNT(ef_skipped); return; }
    }
    pthread_cond_broadcast(&ef->cond);
    if (ef->first) EF_COUNT(ef_wakeups);
}
static ABI int32_t ef_create(EventFlag **out,const char *name,uint32_t attr,uint64_t init,const void *param) {
    (void)param;
    if (!out || (attr & ~UINT32_C(0x33))) return KERR_EINVAL;
    EventFlag *ef=calloc(1,sizeof(*ef));
    if (!ef) return KERR_ENOMEM;
    ef->magic=EF_MAGIC; ef->attr=attr; ef->bits=init;
    pthread_mutex_init(&ef->lock,NULL);
    pthread_cond_init(&ef->cond,NULL);
#ifdef _WIN32
    InitializeSRWLock(&ef->srw);
#endif
    snprintf(ef->name,sizeof(ef->name),"%s",name ? name : "");
    *out=ef; return 0;
}
static int ef_valid(const EventFlag *ef) { return ef && ef->magic==EF_MAGIC && !ef->deleted; }
static ABI int32_t ef_delete(EventFlag *ef) {
    if (!ef_valid(ef)) return KERR_ESRCH;
    ef_lock(ef);
    ef->deleted=1;
    ef_wake(ef,0);
    int busy=ef->waiters;
    ef_unlock(ef);
    /* Waiters return ESRCH; the object is leaked if any were blocked, since they still read it. */
    if (!busy) { ef->magic=0; pthread_cond_destroy(&ef->cond); pthread_mutex_destroy(&ef->lock); free(ef); }
    return 0;
}
static void ef_consume(EventFlag *ef,uint64_t pattern,uint32_t mode,uint64_t *result);
/* TLG_EF_HANDOFF: satisfies the waiters the bits now match, in arrival order (a clear mode can
 * leave later ones unsatisfied), and wakes only those. */
static void ef_hand_off(EventFlag *ef) {
    int any=0;
    for (EfWaiter *w=ef->first;w;w=w->next) {
        if (w->done || !ef_match(ef->bits,w->pattern,w->mode)) continue;
        w->done=1; any=1;
        ef_consume(ef,w->pattern,w->mode,&w->result);
#ifdef _WIN32
        if (ef_srw()) { WakeConditionVariable(&w->cv); EF_COUNT(ef_wakeups); }
#endif
    }
    if (ef_srw()) return;
    if (any) { pthread_cond_broadcast(&ef->cond); EF_COUNT(ef_wakeups); }
    else if (ef->first) EF_COUNT(ef_skipped);
}
static ABI int32_t ef_set(EventFlag *ef,uint64_t bits) {
    if (!ef_valid(ef)) return KERR_ESRCH;
    EF_COUNT(ef_sets);
    ef_lock(ef);
    ef->bits|=bits;
    if (ef_handoff()) ef_hand_off(ef);
    else ef_wake(ef,ef_selective());
    ef_unlock(ef);
    return 0;
}
static ABI int32_t ef_clear(EventFlag *ef,uint64_t mask) {
    if (!ef_valid(ef)) return KERR_ESRCH;
    ef_lock(ef);
    ef->bits&=mask;
    ef_unlock(ef);
    return 0;
}
/* sceKernelCancelEventFlag: the waiters return ECANCELED, the bits become `pattern`. */
static ABI int32_t ef_cancel(EventFlag *ef,uint64_t pattern,int32_t *waiters) {
    if (!ef_valid(ef)) return KERR_ESRCH;
    ef_lock(ef);
    if (waiters) *waiters=ef->waiters;
    ef->bits=pattern;
    ++ef->cancel_generation;
    ef_wake(ef,0);
    ef_unlock(ef);
    return 0;
}
static void ef_consume(EventFlag *ef,uint64_t pattern,uint32_t mode,uint64_t *result) {
    if (result) *result=ef->bits;
    if (mode & EF_CLEAR_ALL) ef->bits=0;
    else if (mode & EF_CLEAR_PAT) ef->bits&=~pattern;
}
static int ef_mode_ok(uint32_t mode) {
    uint32_t wait=mode & 3, clear=mode & 0x30;
    return (wait==EF_WAIT_AND || wait==EF_WAIT_OR) && clear!=0x30 && !(mode & ~UINT32_C(0x33));
}
static ABI int32_t ef_poll(EventFlag *ef,uint64_t pattern,uint32_t mode,uint64_t *result) {
    if (!ef_valid(ef)) return KERR_ESRCH;
    if (!pattern || !ef_mode_ok(mode)) return KERR_EINVAL;
    ef_lock(ef);
    int32_t r=KERR_EBUSY;
    if (!(ef->waiters && !(ef->attr & EF_ATTR_MULTI)) && ef_match(ef->bits,pattern,mode)) {
        ef_consume(ef,pattern,mode,result); r=0;
    } else if (result) *result=ef->bits;
    ef_unlock(ef);
    return r;
}
static uint64_t ef_ns(const struct timespec *t) { return (uint64_t)t->tv_sec*1000000000+(uint64_t)t->tv_nsec; }
/* One sleep of a waiter: 0 when woken (or spuriously), ETIMEDOUT once the deadline has passed. */
static int ef_sleep(EventFlag *ef,EfWaiter *w,const struct timespec *deadline,uint64_t mono_deadline) {
#ifdef _WIN32
    if (ef_srw()) {
        DWORD ms=INFINITE;
        struct timespec t;
        if (deadline) {
            clock_gettime(CLOCK_MONOTONIC,&t);
            uint64_t now=ef_ns(&t);
            if (now>=mono_deadline) return ETIMEDOUT;
            ms=(DWORD)((mono_deadline-now+999999)/1000000);   /* rounded up: never ends early */
        }
        if (SleepConditionVariableSRW(&w->cv,&ef->srw,ms,0)) return 0;
        DWORD e=GetLastError();
        if (e!=ERROR_TIMEOUT) { fprintf(stderr,"STOP: event flag host error %lu\n",(unsigned long)e); exit(21); }
        clock_gettime(CLOCK_MONOTONIC,&t);
        return deadline && ef_ns(&t)>=mono_deadline ? ETIMEDOUT : 0;
    }
#else
    (void)w; (void)mono_deadline;
#endif
    return deadline ? pthread_cond_timedwait(&ef->cond,&ef->lock,deadline) : pthread_cond_wait(&ef->cond,&ef->lock);
}
static ABI int32_t ef_wait(EventFlag *ef,uint64_t pattern,uint32_t mode,uint64_t *result,uint32_t *timeout_us) {
    if (!ef_valid(ef)) return KERR_ESRCH;
    if (!pattern || !ef_mode_ok(mode)) return KERR_EINVAL;
    ef_lock(ef);
    if (ef->waiters && !(ef->attr & EF_ATTR_MULTI)) { ef_unlock(ef); return KERR_EPERM; }
    /* The deadline is CLOCK_REALTIME (the condvar is initialized with the default clock); the
     * time spent is measured with the monotonic clock, which no step can move backwards (a step
     * would make the spent time negative and wrap the remaining timeout below). */
    struct timespec deadline={0,0}, mono_start={0,0};
    if (timeout_us) {
        struct timespec start;
        clock_gettime(CLOCK_REALTIME,&start);
        clock_gettime(CLOCK_MONOTONIC,&mono_start);
        uint64_t ns=(uint64_t)start.tv_nsec+(uint64_t)*timeout_us*1000;
        deadline.tv_sec=start.tv_sec+(time_t)(ns/1000000000); deadline.tv_nsec=(long)(ns%1000000000);
    }
    const uint64_t mono_deadline=timeout_us ? ef_ns(&mono_start)+(uint64_t)*timeout_us*1000 : 0;
    int32_t r=0;
    const int generation=ef->cancel_generation;
    ++ef->waiters;
    EfWaiter w={.pattern=pattern,.mode=mode};
#ifdef _WIN32
    InitializeConditionVariable(&w.cv);
#endif
    EfWaiter **tail=&ef->first;
    while (*tail) tail=&(*tail)->next;
    *tail=&w;
    int blocked=0;
    runtime_wait_begin(timeout_us ? "event flag (timed)" : "event flag",ef,ef->name,NULL,RUNTIME_GUEST_FRAME());
    while (!w.done && !ef->deleted && ef->cancel_generation==generation && !ef_match(ef->bits,pattern,mode)) {
        if (!blocked) { blocked=1; EF_COUNT(ef_blocking); }
        int e=ef_sleep(ef,&w,timeout_us ? &deadline : NULL,mono_deadline);
        if (e==ETIMEDOUT) { r=KERR_ETIMEDOUT; break; }
    }
    runtime_wait_end();
    for (EfWaiter **p=&ef->first;*p;p=&(*p)->next) if (*p==&w) { *p=w.next; break; }
    --ef->waiters;
    if (w.done) { r=0; if (result) *result=w.result; }   /* handed off: already consumed */
    else if (ef->deleted) r=KERR_ESRCH;
    else if (ef->cancel_generation!=generation) { r=KERR_ECANCELED; if (result) *result=ef->bits; }
    else if (!r) ef_consume(ef,pattern,mode,result);
    else if (result) *result=ef->bits;
    ef_unlock(ef);
    if (timeout_us) {
        if (r==KERR_ETIMEDOUT) *timeout_us=0;
        else {
            struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
            int64_t spent=(int64_t)(now.tv_sec-mono_start.tv_sec)*1000000+(now.tv_nsec-mono_start.tv_nsec)/1000;
            if (spent<0) spent=0;
            *timeout_us=spent>=(int64_t)*timeout_us ? 0 : (uint32_t)(*timeout_us-(uint32_t)spent);
        }
    }
    return r;
}
void runtime_event_flag_report(void) {
    if (runtime_counters)
        printf("Runtime: event flags sets=%zu, blocking waits=%zu, wakeups=%zu, wakeups skipped=%zu%s%s\n",
               ef_sets,ef_blocking,ef_wakeups,ef_skipped,
               ef_handoff() ? " (handoff)" : ef_selective() ? " (wake satisfied)" : "",ef_srw() ? " (SRW)" : "");
}

/* ---- Kernel queries ---- */
/* Base PS4 (not Pro): 1080p output path and the 0xb0000000 GPU memory layout. */
static ABI int32_t is_neo_mode(void) { return 0; }
static ABI int32_t available_flexible_memory(uint64_t *size) {
    if (!size) return KERR_EINVAL;
    *size=UINT64_C(448)*1024*1024; return 0;
}
static ABI int32_t getdents(int32_t fd,char *buffer,uint64_t size) {
    return (int32_t)runtime_file_getdents(fd,buffer,size,NULL);
}
static ABI int32_t prt_aperture(int32_t index,void **address,uint64_t *size) {
    if (index<0 || index>2 || !address || !size) return KERR_EINVAL;
    *address=NULL; *size=0; return 0;
}
/* The libc unwinder asks for a module's .eh_frame_hdr when a C++ exception is thrown. No
 * exception has been observed yet; report "no module" so it fails loudly instead of misbehaving. */
static ABI int32_t module_info_for_unwind(uintptr_t address,int32_t flags,void *info) {
    (void)flags; (void)info;
    fprintf(stderr,"Runtime: sceKernelGetModuleInfoForUnwind(%#lx) not implemented (C++ exception?)\n",(unsigned long)address);
    return KERR_ESRCH;
}
static ABI int32_t module_segment_info(void *info) {
    (void)info; once("sceKernelInternalMemoryGetModuleSegmentInfo"); return KERR_ESRCH;
}

/* ---- Time zone ---- */
/* Local time and its offset from UTC in seconds (Windows has no tm_gmtoff: the offset is the
 * local broken-down time read back as UTC, minus the instant). */
static int tlg_localtime(time_t t,struct tm *out,int64_t *offset) {
#ifdef _WIN32
    if (localtime_s(out,&t)) return 0;
    struct tm copy=*out;
    *offset=(int64_t)_mkgmtime(&copy)-(int64_t)t;
#else
    if (!localtime_r(&t,out)) return 0;
    *offset=out->tm_gmtoff;
#endif
    return 1;
}
typedef struct { int64_t t; uint32_t west_sec, dst_sec; } Timesec;
typedef struct { int32_t minutes_west, dst_time; } Timezone;
static ABI int32_t utc_to_local(int64_t utc,int64_t *local,Timesec *ts,uint64_t *dst) {
    struct tm tm; int64_t offset;
    if (!tlg_localtime((time_t)utc,&tm,&offset)) return KERR_EINVAL;
    int64_t dst_sec=tm.tm_isdst>0 ? 3600 : 0;
    if (local) *local=utc+offset;
    if (ts) { ts->t=utc; ts->west_sec=(uint32_t)(offset-dst_sec); ts->dst_sec=(uint32_t)dst_sec; }
    if (dst) *dst=(uint64_t)dst_sec;
    return 0;
}
static ABI int32_t local_to_utc(int64_t local,int64_t unused,int64_t *utc,Timezone *tz,int32_t *dst) {
    (void)unused;
    struct tm tm; int64_t offset;
    if (!tlg_localtime((time_t)local,&tm,&offset)) return KERR_EINVAL;
    if (utc) *utc=local-offset;
    if (tz) { tz->minutes_west=(int32_t)(-offset/60); tz->dst_time=tm.tm_isdst>0; }
    if (dst) *dst=tm.tm_isdst>0 ? 3600 : 0;
    return 0;
}

/* ---- libc internal heap tracing: report no tracing ---- */
typedef struct { uint64_t size; uint32_t flag, get_segment_info; uint64_t *mspace_id_mask, *mstate_table; } HeapTraceInfo;
static ABI int32_t libc_heap_trace_info(HeapTraceInfo *info) {
    static uint64_t mask, table[64];
    if (!info) return -1;
    info->get_segment_info=0; info->mspace_id_mask=&mask; info->mstate_table=table;
    return 0;
}

/* Needed by libSceNgs2.sprx (console firmware module, TLG_NGS2=native). */
static ABI int32_t compiled_sdk_version(int32_t *version) {
    if (!version) return KERR_EINVAL;
    *version = 0x04008061; /* the SDK the game's own modules report (FIOS2 log) */
    return 0;
}
static ABI int32_t posix_pthread_equal(void *a, void *b) { return a == b; }
/* libSceLibcInternal export the game's libc.prx does not carry. */
static ABI void guest_sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }

/* ---- System services ---- */
static ABI int32_t load_exec(const char *path,char *const *argv) {
    printf("Runtime: sceSystemServiceLoadExec(%s%s) -> restart\n",path ? path : "(null)",argv && argv[0] ? " ..." : "");
    fflush(NULL);
    runtime_restart();
    return 0;
}
static ABI int32_t pad_speaker_mix(int32_t handle,int32_t level) { (void)handle; (void)level; return 0; }
static ABI int32_t pad_motion_sensor(int32_t handle,int32_t enable) { (void)handle; (void)enable; return 0; }
#define DISC_MAP_NO_BITMAP ((int32_t)0x81100004)
static ABI int32_t discmap_7c98(const char *path,int64_t offset,int64_t size,int32_t *a,int32_t *b,int32_t *c) {
    (void)path; (void)offset; (void)size; (void)a; (void)b; (void)c; return DISC_MAP_NO_BITMAP;
}

/* PlayGo dialog: only opened when a chunk is missing, which never happens here. */
#define DIALOG_STATUS_FINISHED 3
static ABI int32_t playgo_dialog_init(void) { return 0; }
static ABI int32_t playgo_dialog_open(const void *param) { (void)param; once("scePlayGoDialogOpen"); return 0; }
static ABI int32_t playgo_dialog_status(void) { return DIALOG_STATUS_FINISHED; }
static ABI int32_t playgo_dialog_result(int32_t *result) { if (result) *result=0; return 0; }
static ABI int32_t playgo_dialog_term(void) { return 0; }

/* ---- NGS2: silent placeholder ----
 * Handles are tagged heap objects so the game can keep and compare them; voices report "idle",
 * renders produce silence. No guest allocator is called. */
#define NGS2_MAGIC UINT32_C(0x4e475332)
#define NGS2_INVALID ((int32_t)0x804A0106)
typedef struct { uint32_t magic, kind, rack_id, index; } Ngs2Handle;
typedef struct { void *host_buffer; uint64_t host_size; uintptr_t reserved[5]; uintptr_t user_data; } Ngs2BufferInfo;
typedef struct { void *buffer; uint64_t size; uint32_t waveform, channels; } Ngs2RenderBuffer;
static Ngs2Handle *ngs2_new(uint32_t kind,uint32_t rack_id,uint32_t index) {
    Ngs2Handle *h=calloc(1,sizeof(*h));
    if (h) *h=(Ngs2Handle){NGS2_MAGIC,kind,rack_id,index};
    return h;
}
static int ngs2_valid(const Ngs2Handle *h,uint32_t kind) { return h && h->magic==NGS2_MAGIC && h->kind==kind; }
static ABI int32_t ngs2_system_create(const void *option,const void *allocator,Ngs2Handle **out) {
    (void)option; (void)allocator;
    if (!out || !(*out=ngs2_new(1,0,0))) return NGS2_INVALID;
    once("NGS2 system (silent)"); return 0;
}
static ABI int32_t ngs2_system_destroy(Ngs2Handle *h,Ngs2BufferInfo *info) {
    if (!ngs2_valid(h,1)) return NGS2_INVALID;
    if (info) memset(info,0,sizeof(*info));
    h->magic=0; free(h); return 0;
}
static ABI int32_t ngs2_render(Ngs2Handle *h,const Ngs2RenderBuffer *buffers,uint32_t count) {
    if (!ngs2_valid(h,1) || (count && !buffers)) return NGS2_INVALID;
    for (uint32_t i=0;i<count;++i) if (buffers[i].buffer) memset(buffers[i].buffer,0,buffers[i].size);
    return 0;
}
static ABI int32_t ngs2_query_buffer(uint32_t rack_id,const void *option,Ngs2BufferInfo *info) {
    (void)rack_id; (void)option;
    if (!info) return NGS2_INVALID;
    memset(info,0,sizeof(*info)); info->host_size=4096; return 0;
}
static ABI int32_t ngs2_rack_create(Ngs2Handle *system,uint32_t rack_id,const void *option,const void *buffer,Ngs2Handle **out) {
    (void)option; (void)buffer;
    if (!ngs2_valid(system,1) || !out || !(*out=ngs2_new(2,rack_id,0))) return NGS2_INVALID;
    return 0;
}
static ABI int32_t ngs2_rack_destroy(Ngs2Handle *h,Ngs2BufferInfo *info) {
    if (!ngs2_valid(h,2)) return NGS2_INVALID;
    if (info) memset(info,0,sizeof(*info));
    h->magic=0; free(h); return 0; /* voice handles of this rack are leaked on purpose */
}
static ABI int32_t ngs2_voice_handle(Ngs2Handle *rack,uint32_t index,Ngs2Handle **out) {
    if (!ngs2_valid(rack,2) || !out || !(*out=ngs2_new(3,rack->rack_id,index))) return NGS2_INVALID;
    return 0;
}
static ABI int32_t ngs2_voice_control(Ngs2Handle *voice,const void *params) {
    (void)params; return ngs2_valid(voice,3) ? 0 : NGS2_INVALID;
}
static ABI int32_t ngs2_voice_state(Ngs2Handle *voice,void *state,uint64_t size) {
    if (!ngs2_valid(voice,3) || !state) return NGS2_INVALID;
    memset(state,0,size); return 0; /* stateFlags 0: voice idle */
}

static const RuntimeExport exports[]={
    {"sceKernelCreateEventFlag",ef_create}, {"sceKernelDeleteEventFlag",ef_delete},
    {"sceKernelSetEventFlag",ef_set}, {"sceKernelClearEventFlag",ef_clear},
    {"sceKernelPollEventFlag",ef_poll}, {"sceKernelWaitEventFlag",ef_wait},
    {"sceKernelCancelEventFlag",ef_cancel},
    {"sceKernelIsNeoMode",is_neo_mode}, {"sceKernelAvailableFlexibleMemorySize",available_flexible_memory},
    {"sceKernelGetdents",getdents}, {"sceKernelGetPrtAperture",prt_aperture},
    {"sceKernelGetModuleInfoForUnwind",module_info_for_unwind},
    {"sceKernelInternalMemoryGetModuleSegmentInfo",module_segment_info},
    {"sceKernelConvertUtcToLocaltime",utc_to_local}, {"sceKernelConvertLocaltimeToUtc",local_to_utc},
    {"sceLibcHeapGetTraceInfo",libc_heap_trace_info},
    {"sceKernelGetCompiledSdkVersion",compiled_sdk_version}, {"pthread_equal",posix_pthread_equal},
    {"sincosf",guest_sincosf},
    {"sceSystemServiceLoadExec",load_exec},
    {"sceAudioOutSetMixLevelPadSpk",pad_speaker_mix}, {"scePadSetMotionSensorState",pad_motion_sensor},
    {"sceDiscMap_7C980FFB0AA27E7A",discmap_7c98},
    {"scePlayGoDialogInitialize",playgo_dialog_init}, {"scePlayGoDialogOpen",playgo_dialog_open},
    {"scePlayGoDialogUpdateStatus",playgo_dialog_status}, {"scePlayGoDialogGetResult",playgo_dialog_result},
    {"scePlayGoDialogTerminate",playgo_dialog_term},
    {"sceNgs2SystemCreateWithAllocator",ngs2_system_create}, {"sceNgs2SystemDestroy",ngs2_system_destroy},
    {"sceNgs2SystemRender",ngs2_render}, {"sceNgs2RackQueryBufferSize",ngs2_query_buffer},
    {"sceNgs2RackCreate",ngs2_rack_create}, {"sceNgs2RackCreateWithAllocator",ngs2_rack_create},
    {"sceNgs2RackDestroy",ngs2_rack_destroy}, {"sceNgs2RackGetVoiceHandle",ngs2_voice_handle},
    {"sceNgs2VoiceControl",ngs2_voice_control}, {"sceNgs2VoiceGetState",ngs2_voice_state},
};
/* TLG_NGS2=native: libSceNgs2.sprx from the user's console firmware is linked as a module
 * (run_tlg.sh), so its exports must win over the silent placeholders above. */
static int ngs2_native(void) {
    static int value = -1;
    if (value < 0) { const char *v = getenv("TLG_NGS2"); value = v && !strcmp(v, "native"); }
    return value;
}
uintptr_t runtime_tlg_resolve(const char *name) {
    if (ngs2_native()) {
        const char *symbol = runtime_symbol(name);
        if (symbol && !strncmp(symbol, "sceNgs2", 7)) return 0;
    }
    return RUNTIME_LOOKUP(exports,name);
}
