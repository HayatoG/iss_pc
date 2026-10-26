/* Kernel semaphores: 32-bit IDs, counted tokens and FIFO waiters.
 * Priority-ordered semaphores are approximated by FIFO order. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
typedef struct Waiter {
    int32_t need, result;
    int done;
    pthread_cond_t event;
#ifdef _WIN32
    CONDITION_VARIABLE cv;   /* TLG_SEMA_SRW */
#endif
    struct Waiter *next;
} Waiter;
typedef struct Sema {
    uint32_t id, attr;
    int32_t count, initial, maximum;
    unsigned active;
    int deleted;
    char name[32];
    Waiter *first;
    struct Sema *next;
} Sema;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static Sema *registry;
static uint32_t next_id=1;
static size_t created, deleted, acquired, signaled, timed_out, blocked;
/* TLG_SEMA_SRW=1 (Windows): the registry lock is an SRWLOCK and each blocked waiter sleeps on its
 * own CONDITION_VARIABLE, which needs no setup, instead of a winpthreads mutex plus a condition
 * variable created and destroyed for every blocking wait. Deadlines then use the monotonic clock.
 * Waiter order, token reservation, timeouts, cancel and delete are the same code either way. */
#ifdef _WIN32
static SRWLOCK srw=SRWLOCK_INIT;
static int srw_mode=-1;
static int use_srw(void) {
    int v=__atomic_load_n(&srw_mode,__ATOMIC_RELAXED);
    if (v<0) {
        const char *e=getenv("TLG_SEMA_SRW");
        v=e && e[0]=='1';
        __atomic_store_n(&srw_mode,v,__ATOMIC_RELAXED);
    }
    return v;
}
static uint64_t mono_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
/* 0 when woken (or spuriously), ETIMEDOUT once the deadline (0: none) has passed. The wait is
 * rounded up to whole milliseconds, so it never ends early. */
static int srw_sleep(CONDITION_VARIABLE *cv,uint64_t deadline) {
    DWORD ms=INFINITE;
    if (deadline) {
        uint64_t now=mono_ns();
        if (now>=deadline) return ETIMEDOUT;
        ms=(DWORD)((deadline-now+999999)/1000000);
    }
    if (SleepConditionVariableSRW(cv,&srw,ms,0)) return 0;
    DWORD e=GetLastError();
    if (e!=ERROR_TIMEOUT) { fprintf(stderr,"STOP: semaphore host error %lu\n",(unsigned long)e); exit(21); }
    return deadline && mono_ns()>=deadline ? ETIMEDOUT : 0;
}
#define LOCK() (use_srw() ? AcquireSRWLockExclusive(&srw) : (void)pthread_mutex_lock(&lock))
#define UNLOCK() (use_srw() ? ReleaseSRWLockExclusive(&srw) : (void)pthread_mutex_unlock(&lock))
#else
static int use_srw(void) { return 0; }
#define LOCK() pthread_mutex_lock(&lock)
#define UNLOCK() pthread_mutex_unlock(&lock)
#endif
static Sema *find(uint32_t id) {
    for (Sema *s=registry;s;s=s->next) if (s->id==id) return s;
    return NULL;
}
static void host_check(int e) {
    if (e) { fprintf(stderr,"STOP: semaphore host error %d\n",e); exit(21); }
}
/* Waits use this clock for their deadlines. winpthreads condition variables accept only
 * CLOCK_REALTIME (pthread_condattr_setclock(CLOCK_MONOTONIC) fails with EINVAL). */
#ifdef _WIN32
#define WAIT_CLOCK CLOCK_REALTIME
#else
#define WAIT_CLOCK CLOCK_MONOTONIC
#endif
static uint64_t now_ns(void) {
#ifdef _WIN32
    if (use_srw()) return mono_ns();
#endif
    struct timespec t;
    if (clock_gettime(WAIT_CLOCK,&t)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static void wake(Waiter *w) {
#ifdef _WIN32
    if (use_srw()) { WakeConditionVariable(&w->cv); return; }
#endif
    host_check(pthread_cond_signal(&w->event));
}
static ABI int32_t sem_create(uint32_t *out,const char *name,uint32_t attr,
                              int32_t initial,int32_t maximum,const void *options) {
    if (!out || !name || attr>2 || initial<0 || maximum<=0 || initial>maximum) return ERR(22);
    if (options) { fputs("STOP: semaphore options are not implemented\n",stderr); exit(21); }
    Sema *s=calloc(1,sizeof(*s));
    if (!s) return ERR(12);
    LOCK();
    if (next_id==UINT32_MAX) { UNLOCK(); free(s); return ERR(28); }
    snprintf(s->name,sizeof(s->name),"%s",name);
    s->id=next_id++; s->attr=attr; s->count=s->initial=initial; s->maximum=maximum;
    s->next=registry; registry=s; *out=s->id; ++created;
    printf("Runtime: semaphore id=%u, attr=%u, initial=%d, max=%d\n",s->id,attr,initial,maximum);
    if (runtime_verbose()) printf("Verbose: semaphore id=%u name \"%s\" created (%zu so far)\n",s->id,s->name,created);
    UNLOCK();
    return 0;
}
static int32_t wait_count(uint32_t id,int32_t need,uint32_t *timeout,int block,uintptr_t frame) {
    LOCK();
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (need<=0 || need>s->maximum) result=ERR(22);
    else if (s->count>=need) { s->count-=need; ++acquired; }
    else if (!block) result=ERR(16);
    else if (timeout && !*timeout) {
        result=ERR(60); ++timed_out;
        if (runtime_verbose() && (timed_out<=64 || !(timed_out%1000))) fprintf(stderr,"Verbose: semaphore %u (%s) timed out at once (zero timeout), timeout #%zu\n",id,s->name,timed_out);
    }
    else {
        /* Priority-ordered semaphores (attr 2) are served FIFO: there is no
           PS4 priority scheduler, host threads run at equal priority. */
        static int trace_sema=-1;
        static unsigned verbose_waits;
        if (trace_sema<0) trace_sema=getenv("TLG_TRACE_SEMA")!=NULL;
        /* Detailed log mode caps this trace at 2000 lines; TLG_TRACE_SEMA alone stays unlimited. */
        if (trace_sema && (!runtime_verbose() || ++verbose_waits<=2000)) fprintf(stderr,"Runtime: blocking wait on semaphore %u (need %d, count %d)\n",id,need,s->count); /* deadlock diagnosis */
        Waiter w={.need=need};
        const int srw_wait=use_srw();
        if (srw_wait) {
#ifdef _WIN32
            InitializeConditionVariable(&w.cv);
#endif
        } else {
            pthread_condattr_t attr;
            host_check(pthread_condattr_init(&attr));
            host_check(pthread_condattr_setclock(&attr,WAIT_CLOCK));
            host_check(pthread_cond_init(&w.event,&attr));
            host_check(pthread_condattr_destroy(&attr));
        }
        ++blocked;
        Waiter **tail=&s->first;
        while (*tail) tail=&(*tail)->next;
        *tail=&w; ++s->active;
        uint64_t deadline=timeout ? now_ns()+(uint64_t)*timeout*1000 : 0;
        struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
        runtime_wait_begin(timeout ? "semaphore (timed)" : "semaphore",(const void *)(uintptr_t)id,s->name,NULL,frame);
        while (!w.done) {
            int e;
#ifdef _WIN32
            if (srw_wait) e=srw_sleep(&w.cv,timeout ? deadline : 0);
            else
#endif
            e=timeout ? pthread_cond_timedwait(&w.event,&lock,&end) : pthread_cond_wait(&w.event,&lock);
            if (e==ETIMEDOUT && !w.done) {
                Waiter **p=&s->first;
                while (*p!=&w) p=&(*p)->next;
                *p=w.next; w.result=ERR(60); w.done=1; ++timed_out;
                if (runtime_verbose() && (timed_out<=64 || !(timed_out%1000))) fprintf(stderr,"Verbose: semaphore %u (%s) wait timed out after %u us, timeout #%zu\n",id,s->name,timeout ? *timeout : 0,timed_out);
            } else if (e!=ETIMEDOUT) host_check(e);
        }
        runtime_wait_end();
        result=w.result;
        if (!result) ++acquired;
        if (timeout) {
            uint64_t now=now_ns();
            *timeout=result ? 0 : (uint32_t)(now>=deadline ? 0 : (deadline-now)/1000);
        }
        if (!srw_wait) host_check(pthread_cond_destroy(&w.event));
        if (!--s->active && s->deleted) free(s);
    }
    UNLOCK();
    return result;
}
static ABI int32_t sem_wait(uint32_t id,int32_t need,uint32_t *timeout) { return wait_count(id,need,timeout,1,RUNTIME_GUEST_FRAME()); }
static ABI int32_t sem_poll(uint32_t id,int32_t need) { return wait_count(id,need,NULL,0,0); }
static ABI int32_t sem_signal(uint32_t id,int32_t count) {
    LOCK();
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (count<=0 || count>s->maximum-s->count) result=ERR(22);
    else {
        s->count+=count; ++signaled;
        Waiter **p=&s->first;
        while (*p) {
            Waiter *w=*p;
            if (w->need>s->count) { p=&w->next; continue; }
            s->count-=w->need; *p=w->next; w->done=1;
            wake(w);
        }
    }
    UNLOCK();
    return result;
}
static int32_t wake_all(Sema *s,int32_t result) {
    int32_t count=0;
    while (s->first) {
        Waiter *w=s->first; s->first=w->next;
        w->result=result; w->done=1; ++count;
        wake(w);
    }
    return count;
}
static ABI int32_t sem_cancel(uint32_t id,int32_t count,int32_t *waiters) {
    LOCK();
    Sema *s=find(id);
    int32_t result=0;
    if (!s) result=ERR(3);
    else if (count>s->maximum) result=ERR(22);
    else {
        int32_t n=wake_all(s,ERR(85));
        if (waiters) *waiters=n;
        s->count=count<0 ? s->initial : count;
    }
    UNLOCK();
    return result;
}
static ABI int32_t sem_delete(uint32_t id) {
    LOCK();
    Sema **p=&registry;
    while (*p && (*p)->id!=id) p=&(*p)->next;
    if (!*p) { UNLOCK(); return ERR(3); }
    Sema *s=*p; *p=s->next; s->deleted=1; ++deleted;
    if (runtime_verbose()) printf("Verbose: semaphore id=%u (%s) deleted, %zu deleted so far\n",id,s->name,deleted);
    wake_all(s,ERR(13));
    if (!s->active) free(s);
    UNLOCK();
    return 0;
}
/* Read-only test/diagnostic snapshot, not exposed to guest code. */
unsigned runtime_sema_waiters(uint32_t id) {
    LOCK();
    Sema *s=find(id); unsigned n=0;
    if (s) for (Waiter *w=s->first;w;w=w->next) ++n;
    UNLOCK();
    return n;
}
uintptr_t runtime_sema_resolve(const char *name) {
    if (!strcmp(name,"188x57JYp0g#libkernel")) return (uintptr_t)sem_create;
    if (!strcmp(name,"Zxa0VhQVTsk#libkernel")) return (uintptr_t)sem_wait;
    if (!strcmp(name,"4czppHBiriw#libkernel")) return (uintptr_t)sem_signal;
    if (!strcmp(name,"12wOHk8ywb0#libkernel")) return (uintptr_t)sem_poll;
    if (!strcmp(name,"4DM06U2BNEY#libkernel")) return (uintptr_t)sem_cancel;
    if (!strcmp(name,"R1Jvn8bSCW8#libkernel")) return (uintptr_t)sem_delete;
    return 0;
}
void runtime_sema_report(void) {
    LOCK();
    printf("Runtime: semaphores created=%zu, deleted=%zu, acquired=%zu, blocking waits=%zu, signals=%zu, timeouts=%zu%s\n",
           created,deleted,acquired,blocked,signaled,timed_out,use_srw() ? " (SRW)" : "");
    UNLOCK();
}

