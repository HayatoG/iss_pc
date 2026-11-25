/* TLG_ENERGY_WATCH=1 (diagnostics, CUSA00223 1.07): hardware write watchpoints on Delsin's energy
 * (hero = *(image+0x1d0cfe0), energy at hero+0x1a1e8) and the missile count (image+0xefacf8+0xdc).
 * Each change is logged as "ENERGY_WATCH:" with the image offset of the instruction after the
 * write and the image-code addresses found on the stack (callers). Off unless the variable is 1. */
#ifndef ISS_ENERGY_WATCH_H
#define ISS_ENERGY_WATCH_H
#ifdef _WIN32
#include <tlhelp32.h>

typedef struct { volatile LONG done; uint32_t slot, before, after; DWORD tid; uint64_t rip, ret[8]; } EwHit;
enum { EW_RING = 4096, EW_CODE = 0xcc55b8 };
static unsigned char *ew_image;
static volatile uintptr_t ew_addr[2];
static volatile uint32_t ew_last[2];
static EwHit ew_ring[EW_RING];
static volatile LONG ew_head, ew_tail;

static LONG CALLBACK ew_handler(EXCEPTION_POINTERS *info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = info->ContextRecord;
    DWORD64 dr6 = c->Dr6;
    if (!(dr6 & 3)) return EXCEPTION_CONTINUE_SEARCH;
    for (uint32_t s = 0; s < 2; ++s) {
        if (!(dr6 & (1u << s)) || !ew_addr[s]) continue;
        uint32_t now = *(volatile uint32_t *)ew_addr[s], before = ew_last[s];
        if (now == before) continue;
        ew_last[s] = now;
        LONG i = InterlockedIncrement(&ew_head) - 1;
        if (i - ew_tail >= EW_RING) continue;
        EwHit *h = &ew_ring[i % EW_RING];
        h->slot = s; h->before = before; h->after = now; h->tid = GetCurrentThreadId();
        h->rip = c->Rip - (uintptr_t)ew_image;
        const uint64_t *sp = (const uint64_t *)c->Rsp;
        int n = 0;
        for (int k = 0; k < 48 && n < 8; ++k)
            if (sp[k] - (uintptr_t)ew_image < EW_CODE) h->ret[n++] = sp[k] - (uintptr_t)ew_image;
        for (; n < 8; ++n) h->ret[n] = 0;
        InterlockedExchange(&h->done, 1);
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void ew_value(char *out, size_t size, uint32_t slot, uint32_t bits) {
    if (slot == 0) { float f; memcpy(&f, &bits, 4); snprintf(out, size, "%.3f", f); }
    else snprintf(out, size, "%u", bits);
}

static DWORD WINAPI ew_thread(LPVOID unused) {
    (void)unused;
    static DWORD known[4096];
    int nknown = 0;
    uintptr_t applied[2] = {1, 1};
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    for (;;) {
        Sleep(300);
        uintptr_t hero;
        memcpy(&hero, ew_image + 0x1d0cfe0, sizeof(hero));
        uintptr_t want[2] = {(hero >= 0x1000000000ull && hero < 0x10000000000ull) ? hero + 0x1a1e8 : 0,
                             (uintptr_t)ew_image + 0xefacf8 + 0xdc};
        if (want[0] != applied[0] || want[1] != applied[1]) {
            for (int s = 0; s < 2; ++s) { ew_addr[s] = want[s]; ew_last[s] = want[s] ? *(volatile uint32_t *)want[s] : 0; }
            applied[0] = want[0]; applied[1] = want[1]; nknown = 0;
            printf("ENERGY_WATCH: hero=0x%llx watching energy=0x%llx missiles=0x%llx\n",
                   (unsigned long long)hero, (unsigned long long)want[0], (unsigned long long)want[1]);
            fflush(stdout);
        }
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te; te.dwSize = sizeof(te);
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
                int seen = 0;
                for (int k = 0; k < nknown; ++k) if (known[k] == te.th32ThreadID) { seen = 1; break; }
                if (seen || nknown >= 4096) continue;
                HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (!t) continue;
                if (SuspendThread(t) != (DWORD)-1) {
                    CONTEXT c; memset(&c, 0, sizeof(c)); c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (GetThreadContext(t, &c)) {
                        c.Dr0 = want[0]; c.Dr1 = want[1];
                        c.Dr7 = (want[0] ? (1u | (1u << 16) | (3u << 18)) : 0) | (1u << 2) | (1u << 20) | (3u << 22);
                        if (SetThreadContext(t, &c)) known[nknown++] = te.th32ThreadID;
                    }
                    ResumeThread(t);
                }
                CloseHandle(t);
            }
            CloseHandle(snap);
        }
        while (ew_tail < ew_head) {
            EwHit *h = &ew_ring[ew_tail % EW_RING];
            if (!h->done) break;
            char a[32], b[32];
            ew_value(a, sizeof(a), h->slot, h->before); ew_value(b, sizeof(b), h->slot, h->after);
            printf("ENERGY_WATCH: %s %s -> %s after 0x%llx tid %lu callers", h->slot ? "missiles" : "energy", a, b,
                   (unsigned long long)h->rip, (unsigned long)h->tid);
            for (int k = 0; k < 8 && h->ret[k]; ++k) printf(" 0x%llx", (unsigned long long)h->ret[k]);
            printf("\n");
            h->done = 0;
            InterlockedIncrement(&ew_tail);
        }
        fflush(stdout);
    }
    return 0;
}

static void iss_energy_watch_start(unsigned char *image_base) {
    const char *on = getenv("TLG_ENERGY_WATCH");
    if (!on || strcmp(on, "1")) return;
    ew_image = image_base;
    AddVectoredExceptionHandler(1, ew_handler);
    HANDLE t = CreateThread(NULL, 0, ew_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    puts("ENERGY_WATCH: on");
}
#else
static void iss_energy_watch_start(unsigned char *image_base) { (void)image_base; }
#endif
#endif
