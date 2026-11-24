/* Freeze report: when the game has not shown a new frame for TLG_FREEZE_REPORT_SECONDS
 * (default 10, 0 = off) a monitor thread writes, once per freeze, every thread of the process to
 * the log: its name, what it is blocked on (the wait slots of runtime_thread.c, marked by the
 * runtime's and the GPU library's blocking calls), where it is now (RIP) and its frame chain,
 * in guest offsets inside the game image. The game keeps running; the next report needs frames
 * to resume and stop again.
 * Registers: Linux sends SIGURG to each thread (the handler only copies its registers and frame
 * chain); Windows suspends each thread for GetThreadContext. Memory is read with
 * process_vm_readv / ReadProcessMemory, so a bad frame pointer cannot fault. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/tlggpu.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

#define MAX_THREADS 1024
#define MAX_FRAMES 12

static uintptr_t image_base;
static uint64_t image_bytes;

/* Reads that fail instead of faulting (other threads' stacks, freed objects). */
static int safe_read(void *out, uintptr_t address, size_t size) {
    if (!address) return 0;
#ifdef _WIN32
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void *)address, out, size, &got) && got == size;
#else
    struct iovec local = {out, size}, remote = {(void *)address, size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)size;
#endif
}

/* Return addresses along a frame-pointer chain starting at frame (guest code keeps frame pointers). */
static unsigned walk_frames(uintptr_t frame, uintptr_t *out, unsigned max) {
    unsigned n = 0;
    while (n < max && frame) {
        uintptr_t pair[2];
        if (!safe_read(pair, frame, sizeof(pair)) || !pair[1]) break;
        out[n++] = pair[1];
        if (pair[0] <= frame || pair[0] - frame > (64u << 20)) break;
        frame = pair[0];
    }
    return n;
}

static void describe_address(char *out, size_t size, uintptr_t address) {
    if (image_base && address - image_base < image_bytes) {
        snprintf(out, size, "guest+0x%llx", (unsigned long long)(address - image_base));
        return;
    }
#ifdef _WIN32
    HMODULE module = NULL;
    char name[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)address, &module) && module && GetModuleFileNameA(module, name, sizeof(name))) {
        const char *base = strrchr(name, '\\');
        snprintf(out, size, "%s+0x%llx", base ? base + 1 : name, (unsigned long long)(address - (uintptr_t)module));
        return;
    }
#else
    Dl_info where;
    if (dladdr((void *)address, &where) && where.dli_fname) {
        const char *base = strrchr(where.dli_fname, '/');
        snprintf(out, size, "%s+0x%llx%s%s%s", base ? base + 1 : where.dli_fname,
                 (unsigned long long)(address - (uintptr_t)where.dli_fbase),
                 where.dli_sname ? " (" : "", where.dli_sname ? where.dli_sname : "", where.dli_sname ? ")" : "");
        return;
    }
#endif
    snprintf(out, size, "0x%llx", (unsigned long long)address);
}

/* ---- Text that grows (the whole report is written at once) ---- */
typedef struct { char *data; size_t length, capacity; } Text;
static void add(Text *t, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void add(Text *t, const char *format, ...) {
    for (;;) {
        va_list args;
        va_start(args, format);
        size_t room = t->capacity - t->length;
        int n = vsnprintf(t->data ? t->data + t->length : NULL, t->data ? room : 0, format, args);
        va_end(args);
        if (n < 0) return;
        if ((size_t)n < room) { t->length += (size_t)n; return; }
        size_t capacity = t->capacity ? t->capacity * 2 : 16384;
        while (capacity - t->length <= (size_t)n) capacity *= 2;
        char *grown = realloc(t->data, capacity);
        if (!grown) return;
        t->data = grown; t->capacity = capacity;
    }
}

/* ---- Wait description (unit-tested) ---- */
static const RuntimeThreadInfo *find_record(const RuntimeThreadInfo *threads, size_t count, const void *record) {
    for (size_t i = 0; i < count; ++i) if (threads[i].record && threads[i].record == record) return &threads[i];
    return NULL;
}
static void describe_level(Text *t, const RuntimeWaitLevel *l, uint64_t now_ms,
                           const RuntimeThreadInfo *threads, size_t count) {
    uint64_t ms = now_ms >= l->since_ms ? now_ms - l->since_ms : 0;
    add(t, "%s", l->kind);
    if (l->name[0]) add(t, " '%s'", l->name);
    if (!strncmp(l->kind, "sleep", 5)) add(t, " of %llu us", (unsigned long long)(uintptr_t)l->object);
    else if (!strncmp(l->kind, "file", 4) && (uintptr_t)l->object < 0x10000) add(t, " (fd %u)", (unsigned)(uintptr_t)l->object);
    else if (!strncmp(l->kind, "semaphore", 9)) add(t, " id %llu", (unsigned long long)(uintptr_t)l->object);
    else if (l->object) add(t, " 0x%llx", (unsigned long long)(uintptr_t)l->object);
    add(t, " for %llu ms", (unsigned long long)ms);
    if (l->owner) {
        const void *holder = NULL;
        if (!safe_read(&holder, (uintptr_t)l->owner, sizeof(holder))) add(t, ", holder unknown");
        else if (!holder) add(t, ", not held now");
        else if (holder == (const void *)1) add(t, ", held by a host thread");
        else {
            const RuntimeThreadInfo *h = find_record(threads, count, holder);
            if (h) add(t, ", held by '%s' (tid %llu)", h->name, (unsigned long long)h->os_tid);
            else add(t, ", held by thread record 0x%llx", (unsigned long long)(uintptr_t)holder);
        }
    }
}
static void describe_wait(Text *t, const RuntimeWait *w, uint64_t now_ms, const RuntimeThreadInfo *threads, size_t count) {
    unsigned depth = w->depth < RUNTIME_WAIT_LEVELS ? w->depth : RUNTIME_WAIT_LEVELS;
    int shown = 0;
    /* Innermost first: what the thread is blocked on right now, then what it was doing. */
    for (unsigned i = depth; i-- > 0;) {
        const RuntimeWaitLevel *l = &w->level[i];
        if (!l->kind) continue;
        add(t, shown ? " (inside " : "waiting on ");
        describe_level(t, l, now_ms, threads, count);
        if (shown) add(t, ")");
        shown = 1;
    }
    if (!shown) add(t, "running (not in a marked wait)");
    if (w->depth > RUNTIME_WAIT_LEVELS) add(t, " (+%u nested waits)", w->depth - RUNTIME_WAIT_LEVELS);
}
void runtime_freeze_describe_wait(char *out, size_t size, const RuntimeWait *wait, uint64_t now_ms,
                                  const RuntimeThreadInfo *threads, size_t count) {
    Text t = {0};
    describe_wait(&t, wait, now_ms, threads, count);
    snprintf(out, size, "%s", t.data ? t.data : "");
    free(t.data);
}

int runtime_freeze_step(FreezeMonitor *m, uint64_t frames, uint64_t now_ms) {
    if (frames != m->last_frames) {
        const int resumed = m->reported;
        m->freeze_ms = now_ms - m->last_change_ms;
        m->last_frames = frames; m->last_change_ms = now_ms; m->reported = 0;
        return resumed ? 2 : 0;
    }
    /* Armed by the first frame: loading before it is not a freeze. */
    if (!frames || !m->seconds || m->reported) return 0;
    if (now_ms - m->last_change_ms < m->seconds * 1000) return 0;
    m->reported = 1;
    return 1;
}

/* ---- Registers of every thread ---- */
typedef struct {
    uint64_t tid;
    int captured;
    uintptr_t rip, rsp, rbp;
    unsigned frames;
    uintptr_t chain[MAX_FRAMES];
    char name[64];
} Capture;

static uint64_t own_tid(void) {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return (uint64_t)gettid();
#endif
}

#ifndef _WIN32
static pid_t capture_tid;
static Capture *capture_out;
static int capture_done;
/* Async-signal-safe: registers and the frame chain into the capture the monitor waits on. */
static void capture_signal(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info;
    int saved = errno;
    if (__atomic_load_n(&capture_tid, __ATOMIC_ACQUIRE) == gettid()) {
        Capture *c = __atomic_load_n(&capture_out, __ATOMIC_ACQUIRE);
        const greg_t *g = ((ucontext_t *)context)->uc_mcontext.gregs;
        c->rip = (uintptr_t)g[REG_RIP]; c->rsp = (uintptr_t)g[REG_RSP]; c->rbp = (uintptr_t)g[REG_RBP];
        c->frames = walk_frames(c->rbp, c->chain, MAX_FRAMES);
        c->captured = 1;
        __atomic_store_n(&capture_done, 1, __ATOMIC_RELEASE);
    }
    errno = saved;
}
static size_t list_threads(Capture *out, size_t max) {
    size_t n = 0;
    DIR *dir = opendir("/proc/self/task");
    struct dirent *e;
    while (dir && n < max && (e = readdir(dir))) {
        long tid = strtol(e->d_name, NULL, 10);
        if (tid <= 0) continue;
        out[n] = (Capture){.tid = (uint64_t)tid};
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/task/%ld/comm", tid);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            ssize_t got = read(fd, out[n].name, sizeof(out[n].name) - 1);
            close(fd);
            if (got > 0) out[n].name[got - (out[n].name[got - 1] == '\n')] = 0;
        }
        ++n;
    }
    if (dir) closedir(dir);
    return n;
}
static void capture(Capture *c) {
    __atomic_store_n(&capture_done, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&capture_out, c, __ATOMIC_RELEASE);
    __atomic_store_n(&capture_tid, (pid_t)c->tid, __ATOMIC_RELEASE);
    if (!syscall(SYS_tgkill, getpid(), (pid_t)c->tid, SIGURG)) {
        /* A thread that blocks SIGURG never answers: 30 ms each at most. */
        for (int i = 0; i < 60 && !__atomic_load_n(&capture_done, __ATOMIC_ACQUIRE); ++i) {
            struct timespec nap = {0, 500000};
            nanosleep(&nap, NULL);
        }
    }
    __atomic_store_n(&capture_tid, 0, __ATOMIC_RELEASE);
}
#else
typedef HRESULT (WINAPI *GetDescription)(HANDLE, PWSTR *);
static size_t list_threads(Capture *out, size_t max) {
    size_t n = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    static GetDescription get_description;
    if (!get_description)
        get_description = (GetDescription)(void (*)(void))GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    THREADENTRY32 entry = {.dwSize = sizeof(entry)};
    const DWORD process = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snapshot, &entry); ok && n < max; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process) continue;
        out[n] = (Capture){.tid = entry.th32ThreadID};
        HANDLE thread = get_description ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID) : NULL;
        PWSTR wide = NULL;
        if (thread && SUCCEEDED(get_description(thread, &wide)) && wide) {
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, out[n].name, sizeof(out[n].name), NULL, NULL);
            LocalFree(wide);
        }
        if (thread) CloseHandle(thread);
        ++n;
    }
    CloseHandle(snapshot);
    return n;
}
/* Suspended only for GetThreadContext and the frame reads (no locks are taken meanwhile). */
static void capture(Capture *c) {
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)c->tid);
    if (!thread) return;
    if (SuspendThread(thread) != (DWORD)-1) {
        static CONTEXT context __attribute__((aligned(16)));
        memset(&context, 0, sizeof(context));
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(thread, &context)) {
            c->rip = (uintptr_t)context.Rip; c->rsp = (uintptr_t)context.Rsp; c->rbp = (uintptr_t)context.Rbp;
            c->frames = walk_frames(c->rbp, c->chain, MAX_FRAMES);
            c->captured = 1;
        }
        ResumeThread(thread);
    }
    CloseHandle(thread);
}
#endif

static void add_chain(Text *t, const uintptr_t *chain, unsigned frames) {
    char where[320];
    for (unsigned i = 0; i < frames; ++i) {
        describe_address(where, sizeof(where), chain[i]);
        add(t, "%s%s", i ? " < " : "", where);
    }
}
static void add_thread(Text *t, const Capture *c, const RuntimeThreadInfo *info, uint64_t now_ms,
                       const RuntimeThreadInfo *threads, size_t count) {
    char where[320];
    if (info && info->record) add(t, "  guest '%s' (tid %llu): ", info->name, (unsigned long long)c->tid);
    else add(t, "  host '%s' (tid %llu): ", c->name[0] ? c->name : "?", (unsigned long long)c->tid);
    if (info) describe_wait(t, &info->wait, now_ms, threads, count);
    else add(t, "no marked wait");
    add(t, "\n");
    if (c->captured) {
        describe_address(where, sizeof(where), c->rip);
        add(t, "      now at %s, rsp=0x%llx", where, (unsigned long long)c->rsp);
        if (c->frames) { add(t, "; frames: "); add_chain(t, c->chain, c->frames); }
        add(t, "\n");
    } else {
        add(t, "      registers not captured (the thread did not answer)\n");
    }
    /* The guest frames that called the marked wait (the import's own frame starts the chain). */
    if (info && info->wait.depth && info->wait.level[0].kind && info->wait.level[0].frame) {
        uintptr_t chain[MAX_FRAMES];
        unsigned frames = walk_frames(info->wait.level[0].frame, chain, MAX_FRAMES);
        if (frames) { add(t, "      wait called from "); add_chain(t, chain, frames); add(t, "\n"); }
    }
}

static TlgGpuFrameCounters counters_at_last_frame;
static void add_counters(Text *t) {
    TlgGpuFrameCounters now;
    tlggpu_frame_counters(&now);
    const TlgGpuFrameCounters *then = &counters_at_last_frame;
    add(t, "  frames: %llu flips submitted by the game (+%llu since the last shown frame), %llu shown, "
           "%llu presented (+%llu), %llu vblank ticks (+%llu); page faults being handled now: %llu (%llu handled, +%llu)\n",
        (unsigned long long)now.submitted, (unsigned long long)(now.submitted - then->submitted),
        (unsigned long long)now.shown, (unsigned long long)now.presented,
        (unsigned long long)(now.presented - then->presented), (unsigned long long)now.vblanks,
        (unsigned long long)(now.vblanks - then->vblanks), (unsigned long long)now.faults_in_progress,
        (unsigned long long)now.faults_handled, (unsigned long long)(now.faults_handled - then->faults_handled));
}

static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;
void runtime_freeze_report(FILE *out, const char *head) {
    pthread_mutex_lock(&report_lock);
    Capture *caps = calloc(MAX_THREADS, sizeof(*caps));
    RuntimeThreadInfo *infos = calloc(MAX_THREADS, sizeof(*infos));
    Text t = {0};
    if (!caps || !infos) { free(caps); free(infos); pthread_mutex_unlock(&report_lock); return; }
    const size_t threads = list_threads(caps, MAX_THREADS);
    const uint64_t self = own_tid();
    for (size_t i = 0; i < threads; ++i) if (caps[i].tid != self) capture(&caps[i]);
    /* Wait slots after the registers: the waits shown are at least as old as the RIPs. */
    const uint64_t now_ms = runtime_wait_now_ms();
    const size_t records = runtime_thread_snapshot(infos, MAX_THREADS);
    add(&t, "%s", head);
    add_counters(&t);
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < threads; ++i) {
            if (caps[i].tid == self) continue;
            /* A live guest record with this OS thread id, else a host wait slot (ids are reused). */
            const RuntimeThreadInfo *info = NULL;
            for (size_t r = 0; r < records && !info; ++r)
                if (infos[r].record && !infos[r].finished && infos[r].os_tid == caps[i].tid) info = &infos[r];
            for (size_t r = 0; r < records && !info; ++r)
                if (!infos[r].record && infos[r].os_tid == caps[i].tid) info = &infos[r];
            const int guest = info && info->record;
            if (guest == (pass == 0)) add_thread(&t, &caps[i], info, now_ms, infos, records);
        }
    }
    add(&t, "FREEZE: end of report (%zu threads; the game keeps running)\n", threads);
    if (t.data) { fwrite(t.data, 1, t.length, out); fflush(out); }
    free(t.data); free(caps); free(infos);
    pthread_mutex_unlock(&report_lock);
}

/* ---- Monitor ---- */
static void sleep_ms(unsigned ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec t = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    while (nanosleep(&t, &t) && errno == EINTR) {}
#endif
}
static void *monitor(void *argument) {
    FreezeMonitor *m = argument;
#ifndef _WIN32
    pthread_setname_np(pthread_self(), "tlg:freeze");
#endif
    const unsigned period = m->seconds >= 4 ? 1000 : (unsigned)m->seconds * 250;
    for (;;) {
        sleep_ms(period);
        TlgGpuFrameCounters now;
        tlggpu_frame_counters(&now);
        const uint64_t ms = runtime_wait_now_ms();
        const uint64_t before = m->last_frames;
        switch (runtime_freeze_step(m, now.shown, ms)) {
        case 1: {
            char head[96];
            snprintf(head, sizeof(head), "FREEZE: no frame for %u s; threads:\n", (unsigned)m->seconds);
            runtime_freeze_report(stdout, head);
            break;
        }
        case 2:
            printf("FREEZE: frames resumed after %.1f s\n", m->freeze_ms / 1000.0);
            fflush(stdout);
            break;
        }
        if (now.shown != before) counters_at_last_frame = now;
    }
    return NULL;
}

#ifdef _WIN32
/* Host call stacks of every other thread (x64 unwind tables; a frame without one is taken as a
 * leaf: its return address is at rsp), module+offset or guest+offset per frame. */
static void describe_host_address(char *out, size_t size, DWORD64 address) {
    HMODULE module = NULL;
    char name[MAX_PATH] = "?";
    if (image_base && address >= image_base && address < image_base + image_bytes) {
        snprintf(out, size, "guest+0x%llx", (unsigned long long)(address - image_base));
    } else if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  (LPCSTR)address, &module) && module) {
        GetModuleFileNameA(module, name, sizeof(name));
        const char *base = strrchr(name, '\\');
        snprintf(out, size, "%s+0x%llx", base ? base + 1 : name,
                 (unsigned long long)(address - (DWORD64)module));
    } else {
        snprintf(out, size, "0x%llx", (unsigned long long)address);
    }
}

static void print_host_stacks(void) {
    const DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = {.dwSize = sizeof(THREADENTRY32)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        if (!t) continue;
        char frames[2048];
        size_t used = 0;
        frames[0] = 0;
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT c;
            memset(&c, 0, sizeof(c));
            c.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(t, &c)) {
                int guest_seen = 0;
                for (int depth = 0; depth < 16 && c.Rip && used < sizeof(frames) - 64; ++depth) {
                    char where[MAX_PATH + 32];
                    if (!guest_seen && image_base && c.Rip >= image_base && c.Rip < image_base + image_bytes) {
                        /* Callee-saved registers of the first guest frame (both ABIs keep them). */
                        guest_seen = 1;
                        used += snprintf(frames + used, sizeof(frames) - used,
                                         " [rbx=%llx rbp=%llx r12=%llx r13=%llx r14=%llx r15=%llx]",
                                         (unsigned long long)c.Rbx, (unsigned long long)c.Rbp,
                                         (unsigned long long)c.R12, (unsigned long long)c.R13,
                                         (unsigned long long)c.R14, (unsigned long long)c.R15);
                        /* The game's label wait at guest+0x1f2700: *(*(r13) + 8 * (s32)[r12+292]) < rbx. */
                        const uint64_t off = c.Rip - image_base;
                        if (off >= 0x1f2600 && off < 0x1f2800) {
                            uint64_t array = 0, value = 0;
                            int32_t index = 0;
                            SIZE_T got = 0;
                            if (ReadProcessMemory(GetCurrentProcess(), (void *)c.R13, &array, 8, &got) &&
                                ReadProcessMemory(GetCurrentProcess(), (void *)(c.R12 + 292), &index, 4, &got) &&
                                ReadProcessMemory(GetCurrentProcess(), (void *)(array + 8 * (int64_t)index), &value, 8, &got))
                                used += snprintf(frames + used, sizeof(frames) - used,
                                                 " [LABEL_WAIT addr=%llx index=%d value=%llx want=%llx]",
                                                 (unsigned long long)(array + 8 * (int64_t)index), index,
                                                 (unsigned long long)value, (unsigned long long)c.Rbx);
                        }
                    }
                    describe_host_address(where, sizeof(where), c.Rip);
                    used += snprintf(frames + used, sizeof(frames) - used, " %s", where);
                    DWORD64 base = 0;
                    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &base, NULL);
                    if (fn) {
                        void *handler_data = NULL;
                        DWORD64 frame = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fn, &c, &handler_data,
                                         &frame, NULL);
                    } else {
                        DWORD64 ret = 0;
                        SIZE_T got = 0;
                        if (!ReadProcessMemory(GetCurrentProcess(), (void *)c.Rsp, &ret, sizeof(ret), &got) ||
                            got != sizeof(ret))
                            break;
                        c.Rip = ret;
                        c.Rsp += 8;
                    }
                }
            }
            ResumeThread(t);
        }
        CloseHandle(t);
        printf("HITCH_STACK tid=%lu:%s\n", (unsigned long)te.th32ThreadID, frames);
    }
    CloseHandle(snap);
    fflush(stdout);
}
#endif

/* TLG_HITCH_REPORT_MS=<n> (diagnostics, off by default): the same report for short freezes. When
 * frames were coming quickly and none was shown for n ms, every thread is captured once while the
 * hitch is still going on (at most 20 reports), then the hitch's length is logged when frames
 * resume. Shows what the game waits for in the ~300 ms hitches seen in play.
 * Slow stretches too: after a second of at least 40 frames, three seconds in a row under 20
 * frames each are reported the same way (SLOW:, again every 10 s while it lasts, at most 12),
 * and the stretch's length when 40 frames a second come back. A 4 fps crawl shows a frame every
 * 250 ms, which the hitch report never sees (2026-11-02, after 17 min of play). */
static void *hitch_monitor(void *argument) {
    const unsigned threshold = (unsigned)(uintptr_t)argument;
#ifndef _WIN32
    pthread_setname_np(pthread_self(), "tlg:hitch");
#endif
    uint64_t last_shown = 0, last_change_ms = runtime_wait_now_ms(), smooth_frames = 0;
    unsigned reports = 0;
    int in_hitch = 0;
    uint64_t second_start_ms = last_change_ms, second_start_shown = 0, slow_since_ms = 0;
    uint64_t slow_frames = 0, last_slow_report_ms = 0;
    unsigned slow_seconds = 0, slow_reports = 0, slow_last_fps = 0;
    int slow_armed = 0, in_slow = 0;
    for (;;) {
        sleep_ms(10);
        TlgGpuFrameCounters now;
        tlggpu_frame_counters(&now);
        const uint64_t ms = runtime_wait_now_ms();
        if (ms - second_start_ms >= 1000) {
            const uint64_t frames = now.shown - second_start_shown;
            const unsigned fps = (unsigned)(frames * 1000 / (ms - second_start_ms));
            second_start_ms = ms;
            second_start_shown = now.shown;
            if (fps >= 40) {
                if (in_slow) {
                    printf("SLOW: back to %u fps after %llu s slow (%llu frames)\n", fps,
                           (unsigned long long)((ms - slow_since_ms) / 1000),
                           (unsigned long long)slow_frames);
                    fflush(stdout);
                }
                slow_armed = 1;
                in_slow = 0;
                slow_seconds = 0;
            } else if (slow_armed && fps < 20) {
                if (!slow_seconds) { slow_since_ms = ms - 1000; slow_frames = 0; }
                ++slow_seconds;
                slow_frames += frames;
                slow_last_fps = fps;
            } else if (!in_slow) {
                slow_seconds = 0;
            }
            if (slow_seconds >= 3 && slow_reports < 12 &&
                (!in_slow || ms - last_slow_report_ms >= 10000)) {
                in_slow = 1;
                last_slow_report_ms = ms;
                ++slow_reports;
                char head[128];
                snprintf(head, sizeof(head), "SLOW: %u fps now, %llu s under 20 fps (report %u); threads:\n",
                         slow_last_fps, (unsigned long long)((ms - slow_since_ms) / 1000), slow_reports);
                runtime_freeze_report(stdout, head);
#ifdef _WIN32
                print_host_stacks();
#endif
            }
        }
        if (now.shown != last_shown) {
            if (in_hitch) {
                printf("HITCH: frames resumed after %llu ms\n", (unsigned long long)(ms - last_change_ms));
                fflush(stdout);
                in_hitch = 0;
                smooth_frames = 0;
            } else if (ms - last_change_ms < 50) {
                smooth_frames += now.shown - last_shown;
            } else {
                smooth_frames = 0;
            }
            last_shown = now.shown;
            last_change_ms = ms;
            continue;
        }
        /* Only after a few seconds of normal frames: not loading screens or menus. */
        if (!in_hitch && smooth_frames >= 120 && ms - last_change_ms >= threshold) {
            in_hitch = 1;
            if (reports < 20) {
                ++reports;
                char head[96];
                snprintf(head, sizeof(head), "HITCH: no frame for %llu ms (report %u); threads:\n",
                         (unsigned long long)(ms - last_change_ms), reports);
                runtime_freeze_report(stdout, head);
#ifdef _WIN32
                print_host_stacks();
#endif
            }
        }
    }
    return NULL;
}

/* RAM report (2026-11-14): every TLG_RAM_REPORT_SECONDS (default 15, 0 = off) one line with this
 * process's RAM (in physical memory now, its private commit, the peak) and the whole PC's, so a log
 * shows whether RAM climbs (a leak) or sits at a fixed level, next to the VRAM lines. Read only. */
static void *ram_monitor(void *arg) {
    const unsigned seconds = (unsigned)(uintptr_t)arg;
    for (;;) {
#ifdef _WIN32
        Sleep(seconds * 1000u);
        PROCESS_MEMORY_COUNTERS_EX pm = {.cb = sizeof(pm)};
        MEMORYSTATUSEX sys = {.dwLength = sizeof(sys)};
        if (!K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof(pm)) ||
            !GlobalMemoryStatusEx(&sys)) continue;
        printf("RAM_USAGE process_in_ram=%llu MB process_committed=%llu MB process_peak_in_ram=%llu MB "
               "pc_used=%llu MB pc_total=%llu MB pc_commit=%llu MB pc_commit_limit=%llu MB\n",
               (unsigned long long)(pm.WorkingSetSize >> 20), (unsigned long long)(pm.PrivateUsage >> 20),
               (unsigned long long)(pm.PeakWorkingSetSize >> 20),
               (unsigned long long)((sys.ullTotalPhys - sys.ullAvailPhys) >> 20),
               (unsigned long long)(sys.ullTotalPhys >> 20),
               (unsigned long long)((sys.ullTotalPageFile - sys.ullAvailPageFile) >> 20),
               (unsigned long long)(sys.ullTotalPageFile >> 20));
        /* Where it is (2026-11-14): committed memory by kind, the guest's PS4 memory window
         * (0x1000000000-0x1200000000) apart, private memory in allocations under 1 MB (heaps)
         * apart, and the five largest private allocations with their committed size. */
        {
            enum { TOP = 5 };
            unsigned long long guest = 0, priv = 0, small = 0, mapped = 0, image = 0;
            unsigned long long top_size[TOP] = {0}; uintptr_t top_base[TOP] = {0};
            uintptr_t addr = 0, cur_base = 0; unsigned long long cur_commit = 0, cur_total = 0;
            MEMORY_BASIC_INFORMATION mbi;
            #define RAM_FLUSH_ALLOC() do { \
                if (cur_base) { \
                    if (cur_total < (1ull << 20)) small += cur_commit; \
                    for (int k = 0; k < TOP; ++k) if (cur_commit > top_size[k]) { \
                        for (int j = TOP - 1; j > k; --j) { top_size[j] = top_size[j-1]; top_base[j] = top_base[j-1]; } \
                        top_size[k] = cur_commit; top_base[k] = cur_base; break; } \
                } } while (0)
            while (addr < (uintptr_t)0x7FFFFFFF0000ull && VirtualQuery((void *)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
                const uintptr_t base = (uintptr_t)mbi.BaseAddress, size = mbi.RegionSize;
                if (mbi.State != MEM_FREE && mbi.Type == MEM_PRIVATE) {
                    const uintptr_t ab = (uintptr_t)mbi.AllocationBase;
                    if (ab != cur_base) { RAM_FLUSH_ALLOC(); cur_base = ab; cur_commit = 0; cur_total = 0; }
                    cur_total += size;
                    if (mbi.State == MEM_COMMIT) cur_commit += size;
                }
                if (mbi.State == MEM_COMMIT) {
                    if (base >= 0x1000000000ull && base < 0x1200000000ull) guest += size;
                    else if (mbi.Type == MEM_PRIVATE) priv += size;
                    else if (mbi.Type == MEM_MAPPED) mapped += size;
                    else if (mbi.Type == MEM_IMAGE) image += size;
                }
                if (base + size <= addr) break;
                addr = base + size;
            }
            RAM_FLUSH_ALLOC();
            #undef RAM_FLUSH_ALLOC
            printf("RAM_REGIONS guest_window=%llu MB private_other=%llu MB of_which_small_allocs=%llu MB mapped=%llu MB modules=%llu MB top=",
                   guest >> 20, priv >> 20, small >> 20, mapped >> 20, image >> 20);
            for (int k = 0; k < TOP; ++k) printf("%s%llx:%lluMB", k ? "," : "", (unsigned long long)top_base[k], top_size[k] >> 20);
            printf("\n");
        }
#ifdef ISS_HEAP_SIZES
        /* Diagnostic build (2026-11-14): once a minute, the process heap (malloc/new) by block
         * size: the sizes holding the most bytes, so a leak shows as one size growing. The walk
         * locks the heap for its duration (a short pause). */
        static unsigned heap_tick;
        if (++heap_tick % 4 == 0) {
            enum { SLOTS = 8192, SHOW = 14 };
            static struct { unsigned long long size, count; } table[SLOTS];
            /* Every heap of the process (the driver's and libraries' own heaps too): busy MB each,
             * then the size table below for the one holding the most. */
            HANDLE heaps[64];
            DWORD nheaps = GetProcessHeaps(64, heaps);
            if (nheaps > 64) nheaps = 64;
            HANDLE biggest = GetProcessHeap(); unsigned long long biggest_bytes = 0;
            printf("HEAP_LIST count=%lu:", (unsigned long)nheaps);
            for (DWORD hi = 0; hi < nheaps; ++hi) {
                unsigned long long hb = 0, hn = 0;
                PROCESS_HEAP_ENTRY he = {0};
                if (!HeapLock(heaps[hi])) continue;
                while (HeapWalk(heaps[hi], &he)) if (he.wFlags & PROCESS_HEAP_ENTRY_BUSY) { hb += he.cbData; ++hn; }
                HeapUnlock(heaps[hi]);
                printf(" %llx%s=%lluMB/%llu", (unsigned long long)(uintptr_t)heaps[hi],
                       heaps[hi] == GetProcessHeap() ? "(process)" : "", hb >> 20, hn);
                if (hb > biggest_bytes) { biggest_bytes = hb; biggest = heaps[hi]; }
            }
            printf("\n");
            memset(table, 0, sizeof(table));
            unsigned long long busy = 0, blocks = 0, overflow = 0;
            HANDLE heap = biggest;
            PROCESS_HEAP_ENTRY e = {0};
            const ULONGLONG t0 = GetTickCount64();
            if (HeapLock(heap)) {
                while (HeapWalk(heap, &e)) {
                    if (!(e.wFlags & PROCESS_HEAP_ENTRY_BUSY)) continue;
                    const unsigned long long s = e.cbData;
                    busy += s; ++blocks;
                    unsigned h = (unsigned)((s * 0x9E3779B97F4A7C15ull) >> 51) & (SLOTS - 1);
                    unsigned probes = 0;
                    while (table[h].count && table[h].size != s && probes++ < SLOTS) h = (h + 1) & (SLOTS - 1);
                    if (probes >= SLOTS) { overflow += s; continue; }
                    table[h].size = s; ++table[h].count;
                }
                HeapUnlock(heap);
            }
            printf("HEAP_SIZES heap=%llx busy=%llu MB blocks=%llu walk_ms=%llu top(size x count = MB):",
                   (unsigned long long)(uintptr_t)heap, busy >> 20, blocks,
                   (unsigned long long)(GetTickCount64() - t0));
            unsigned long long shown[SHOW] = {0}; int nshown = 0;
            for (int k = 0; k < SHOW; ++k) {
                int best = -1; unsigned long long best_bytes = 0;
                for (int i = 0; i < SLOTS; ++i) {
                    const unsigned long long b = table[i].size * table[i].count;
                    if (table[i].count && b > best_bytes) { best_bytes = b; best = i; }
                }
                if (best < 0) break;
                printf(" %llux%llu=%lluMB", table[best].size, table[best].count, best_bytes >> 20);
                shown[nshown++] = table[best].size;
                table[best].count = 0;
            }
            printf("\n");
            /* What the growing blocks are: for the 7 sizes holding the most (under 1 MB), 3 blocks
             * spread over the heap, their first 64 bytes; a value pointing into this exe is printed as
             * exe+offset (vtables, functions: llvm-symbolizer on the diagnostic build names them). */
            {
                const uintptr_t exe = (uintptr_t)GetModuleHandleA(NULL);
                const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(exe + ((const IMAGE_DOS_HEADER *)exe)->e_lfanew);
                const uintptr_t exe_end = exe + nt->OptionalHeader.SizeOfImage;
                for (int k = 0, done = 0; k < nshown && done < 7; ++k) {
                    const unsigned long long want = shown[k];
                    if (want >= (1ull << 20)) continue;
                    ++done;
                    unsigned long long seen = 0, total = 0;
                    PROCESS_HEAP_ENTRY w = {0};
                    if (!HeapLock(heap)) continue;
                    while (HeapWalk(heap, &w)) if ((w.wFlags & PROCESS_HEAP_ENTRY_BUSY) && w.cbData == want) ++total;
                    memset(&w, 0, sizeof(w));
                    const unsigned long long step = total / 3 + 1;
                    while (HeapWalk(heap, &w)) {
                        if (!(w.wFlags & PROCESS_HEAP_ENTRY_BUSY) || w.cbData != want) continue;
                        if (seen++ % step) continue;
                        const unsigned long long *q = (const unsigned long long *)w.lpData;
                        printf("HEAP_SAMPLE size=%llu at=%llx:", want, (unsigned long long)(uintptr_t)w.lpData);
                        for (int j = 0; j < 8; ++j) {
                            const uintptr_t v = (uintptr_t)q[j];
                            if (v >= exe && v < exe_end) printf(" exe+%llx", (unsigned long long)(v - exe));
                            else printf(" %llx", (unsigned long long)v);
                        }
                        printf("\n");
                    }
                    HeapUnlock(heap);
                }
            }
        }
#endif
#else
        sleep(seconds);
        FILE *f = fopen("/proc/self/status", "r");
        if (!f) continue;
        char line[256];unsigned long long rss = 0, hwm = 0, data = 0;
        while (fgets(line, sizeof(line), f)) {
            sscanf(line, "VmRSS: %llu", &rss);sscanf(line, "VmHWM: %llu", &hwm);sscanf(line, "VmData: %llu", &data);
        }
        fclose(f);
        printf("RAM_USAGE process_in_ram=%llu MB process_committed=%llu MB process_peak_in_ram=%llu MB\n",
               rss >> 10, data >> 10, hwm >> 10);
#endif
        fflush(stdout);
    }
    return NULL;
}
static void gpu_wait_begin(const char *kind, const void *object, const char *name, uintptr_t frame) {
    runtime_wait_begin(kind, object, name, NULL, frame);
}
void runtime_freeze_start(uintptr_t image, uint64_t image_size, unsigned seconds) {
    image_base = image; image_bytes = image_size;
    tlggpu_set_wait_hooks(gpu_wait_begin, runtime_wait_end);
#ifndef _WIN32
    struct sigaction action = {0};
    action.sa_sigaction = capture_signal;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGURG, &action, NULL);
#endif
    {
        const char *ram = getenv("TLG_RAM_REPORT_SECONDS");
        const unsigned ram_s = ram && *ram ? (unsigned)strtoul(ram, NULL, 10) : 15;
        pthread_t ram_thread;
        if (ram_s && !pthread_create(&ram_thread, NULL, ram_monitor, (void *)(uintptr_t)ram_s)) {
            pthread_detach(ram_thread);
            printf("RAM report: every %u s (TLG_RAM_REPORT_SECONDS, 0 = off)\n", ram_s);
        }
    }
    {
        const char *hitch = getenv("TLG_HITCH_REPORT_MS");
        const unsigned hitch_ms = hitch ? (unsigned)strtoul(hitch, NULL, 10) : 0;
        if (hitch_ms) {
            pthread_t hitch_thread;
            if (!pthread_create(&hitch_thread, NULL, hitch_monitor, (void *)(uintptr_t)hitch_ms)) {
                pthread_detach(hitch_thread);
                printf("Hitch report: after %u ms without a new frame (TLG_HITCH_REPORT_MS)\n", hitch_ms);
            }
        }
    }
    if (!seconds) return;
    static FreezeMonitor state;
    state = (FreezeMonitor){.seconds = seconds};
    pthread_t thread;
    if (pthread_create(&thread, NULL, monitor, &state)) { puts("Freeze report: monitor thread not started"); return; }
    pthread_detach(thread);
    printf("Freeze report: after %u s without a new frame (TLG_FREEZE_REPORT_SECONDS, 0 = off)\n", seconds);
}
