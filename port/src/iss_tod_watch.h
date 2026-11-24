/* ISS_TOD_WATCH=1 (diagnostics, CUSA00223 1.07, 2026-11-20): the black city. Once a second, the
 * time of day the game wants, the one in use, and the state of its world section, logged as
 * "TOD_WATCH" when they change. Layout from Liondarkz/issn_pc (gpu/shim/issn_exposure.cpp, 0.3.7):
 * state image+0x7f8e800 {current, fallback, wanted id; table count +0x30, buckets +0x38; node
 * {+8 id, +0x10 object, +0x18 next}; name object+0x28 -> +0x58}; world name
 * *(image+0x7f111e0+0x1c0a0)+2; section table image+0x7d37610 (997 buckets at +0x198, node {next,
 * hash, +0x10 index<<8}; objects +0xc8, count +0xd0; flags object+0x44). Memory is read with
 * ReadProcessMemory, so a stale pointer cannot fault.
 * ISS_TOD_FORCE=<name>[@seconds] (test): after <seconds> (default 60) of play, ask the game for the
 * time of day <name> (e.g. time_of_day_dismal) once, as issn_pc's switch does. */
#ifndef ISS_TOD_WATCH_H
#define ISS_TOD_WATCH_H
#ifdef _WIN32
static unsigned char *tw_image;
static int tw_read(uint64_t address, void *out, size_t size) {
    SIZE_T got = 0;
    if (address < 0x10000 || address >= (1ull << 47)) return 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)address, out, size, &got) && got == size;
}
static uint64_t tw_u64(uint64_t a) { uint64_t v = 0; return tw_read(a, &v, 8) ? v : 0; }
static uint32_t tw_u32(uint64_t a) { uint32_t v = 0; return tw_read(a, &v, 4) ? v : 0; }
static void tw_text(uint64_t p, char *out, size_t size) {
    out[0] = 0;
    if (!p) return;
    size_t i = 0;
    for (; i + 1 < size; ++i) {
        char c;
        if (!tw_read(p + i, &c, 1) || !c) break;
        out[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
    }
    out[i] = 0;
}
static void tw_name(uint64_t obj, char *out, size_t size) {
    out[0] = 0;
    const uint64_t data = tw_u64(obj + 0x28);
    if (data) tw_text(tw_u64(data + 0x58), out, size);
}
static uint64_t tw_find(const char *want) {
    const uint64_t s = (uintptr_t)tw_image + 0x7f8e800;
    const uint32_t count = tw_u32(s + 0x30);
    const uint64_t buckets = tw_u64(s + 0x38);
    if (!count || count > 65536 || !buckets) return 0;
    for (uint32_t b = 0; b < count; ++b) {
        uint64_t node = tw_u64(buckets + 8ull * b);
        for (int g = 0; node && g < 64; ++g) {
            char name[64];
            tw_name(tw_u64(node + 0x10), name, sizeof(name));
            if (!strcmp(name, want)) return tw_u64(node + 8);
            node = tw_u64(node + 0x18);
        }
    }
    return 0;
}
static void tw_list(void) {
    const uint64_t s = (uintptr_t)tw_image + 0x7f8e800;
    const uint32_t count = tw_u32(s + 0x30);
    const uint64_t buckets = tw_u64(s + 0x38);
    if (!count || count > 65536 || !buckets) return;
    printf("TOD_WATCH table:");
    for (uint32_t b = 0; b < count; ++b) {
        uint64_t node = tw_u64(buckets + 8ull * b);
        for (int g = 0; node && g < 64; ++g) {
            char name[64];
            tw_name(tw_u64(node + 0x10), name, sizeof(name));
            printf(" %s=%#llx", name[0] ? name : "?", (unsigned long long)tw_u64(node + 8));
            node = tw_u64(node + 0x18);
        }
    }
    printf("\n");
}
/* The section of the wanted time of day: index, object, flags; returns 0 when not in the table. */
static int tw_section(const char *tod, char *section, size_t size, int *index, uint64_t *object, uint8_t flags[4]) {
    char world[64];
    tw_text(tw_u64((uintptr_t)tw_image + 0x7f111e0 + 0x1c0a0) + 2, world, sizeof(world));
    snprintf(section, size, "%s_x_%s", world, tod);
    uint64_t h = 0;
    for (const char *c = section; *c; ++c) h = h * 101 + (uint64_t)tolower((unsigned char)*c);
    const uint64_t table = (uintptr_t)tw_image + 0x7d37610;
    uint64_t node = tw_u64(table + 0x198 + 8ull * ((uint32_t)(h ^ (h >> 32)) % 997u));
    for (int g = 0; node && g < 256 && tw_u64(node + 8) != h; ++g) node = tw_u64(node);
    if (!node || tw_u64(node + 8) != h) return 0;
    int16_t raw = 0;
    tw_read(node + 0x10, &raw, 2);
    *index = raw >> 8;
    const uint32_t count = tw_u32(table + 0xd0);
    const uint64_t array = tw_u64(table + 0xc8);
    *object = (array && (uint32_t)*index < count) ? tw_u64(array + 8ull * (uint32_t)*index) : 0;
    memset(flags, 0, 4);
    if (*object) tw_read(*object + 0x44, flags, 4);
    return 1;
}
/* A package's status in the package manager (image+0x7F77A80, lookup as eboot 0x52AF90: count
 * +0x20, buckets +0x28, node {u32 fold, +8 hash, +0x10 record, +0x18 next}; status = (int8)
 * record+0x10, loaded when > 0). Returns 0x7fff when the manager has no entry for the name. */
static int tw_package(const char *name) {
    uint64_t h = 0;
    for (const char *c = name; *c; ++c) h = h * 101 + (uint64_t)tolower((unsigned char)*c);
    const uint64_t mgr = (uintptr_t)tw_image + 0x7F77A80;
    const uint32_t count = tw_u32(mgr + 0x20);
    const uint64_t buckets = tw_u64(mgr + 0x28);
    if (!count || !buckets) return 0x7ffe;
    const uint32_t fold = (uint32_t)(h >> 32) ^ (uint32_t)h;
    uint64_t node = tw_u64(buckets + 8ull * (fold % count));
    for (int g = 0; node && g < 256; ++g, node = tw_u64(node + 0x18))
        if (tw_u32(node) == fold && tw_u64(node + 8) == h) {
            const uint64_t rec = tw_u64(node + 0x10);
            int8_t st = 0;
            if (!rec || !tw_read(rec + 0x10, &st, 1)) return 0x7ffd;
            return st;
        }
    return 0x7fff;
}
static DWORD WINAPI tw_thread(LPVOID unused) {
    (void)unused;
    const char *force = getenv("ISS_TOD_FORCE");
    char force_name[64] = "";
    int force_after = 60, forced = 0;
    if (force && *force) {
        snprintf(force_name, sizeof(force_name), "%s", force);
        char *at = strchr(force_name, '@');
        if (at) { *at = 0; force_after = atoi(at + 1); }
    }
    uint64_t last_wanted = ~0ull, last_current = ~0ull, last_object = ~0ull;
    uint8_t last_flags[4] = {9, 9, 9, 9};
    int listed = 0, seconds = 0, play = -1;
    for (;; Sleep(1000), ++seconds) {
        const uint64_t s = (uintptr_t)tw_image + 0x7f8e800;
        const uint64_t cur = tw_u64(s), fb = tw_u64(s + 8), wanted = tw_u64(s + 0x10);
        if (!wanted) continue;
        if (play < 0) play = seconds;
        if (!listed) { tw_list(); listed = 1; }
        uint64_t cur_id = ~0ull; int lighting = 0;
        const uint64_t data = cur ? tw_u64(cur + 0x28) : 0;
        if (data) { cur_id = tw_u64(data + 0x18); lighting = tw_u64(data + 0x68) != 0; }
        char wanted_name[64] = "?";
        /* the wanted id's name: find the entry with that id */
        {
            const uint32_t count = tw_u32(s + 0x30);
            const uint64_t buckets = tw_u64(s + 0x38);
            for (uint32_t b = 0; buckets && count <= 65536 && b < count; ++b)
                for (uint64_t node = tw_u64(buckets + 8ull * b); node; node = tw_u64(node + 0x18))
                    if (tw_u64(node + 8) == wanted) { tw_name(tw_u64(node + 0x10), wanted_name, sizeof(wanted_name)); b = count; break; }
        }
        char section[160] = "";
        int index = -1; uint64_t object = 0; uint8_t flags[4] = {0};
        const int has = tw_section(wanted_name, section, sizeof(section), &index, &object, flags);
        static int last_i2 = 0x7000, last_ftp = 0x7000;
        const int i2 = tw_package("install_i2"), ftp = tw_package("msn_ftp");
        if (i2 != last_i2 || ftp != last_ftp) {
            printf("TOD_WATCH %ds: package install_i2 %s%d, msn_ftp %s%d (status > 0 = loaded; 32767 = unknown to the manager)\n",
                   seconds - play, "", i2, "", ftp);
            fflush(stdout);
            last_i2 = i2; last_ftp = ftp;
        }
        if (wanted != last_wanted || cur_id != last_current || object != last_object || memcmp(flags, last_flags, 4)) {
            char cur_name[64] = "none";
            if (data) tw_name(cur, cur_name, sizeof(cur_name));
            printf("TOD_WATCH %ds: wanted '%s' (%#llx); in use '%s'%s%s; section '%s' %s index %d object %#llx flags %u %u %u %u\n",
                   seconds - play, wanted_name, (unsigned long long)wanted, cur_name, lighting ? " (lighting)" : "",
                   cur && cur == fb ? " (fallback)" : "", section, has ? "found" : "NOT in the table", index,
                   (unsigned long long)object, flags[0], flags[1], flags[2], flags[3]);
            if (object) {
                uint8_t raw[0x60];
                if (tw_read(object, raw, sizeof(raw))) {
                    printf("TOD_WATCH section object:");
                    for (int i = 0; i < 0x60; ++i) printf("%s%02x", i % 16 ? "" : " ", raw[i]);
                    printf("\n");
                }
            }
            fflush(stdout);
            last_wanted = wanted; last_current = cur_id; last_object = object; memcpy(last_flags, flags, 4);
        }
        /* ISS_PIECE_UNGATE=1 (test): the wanted time of day's section is installed (PlayGo locus 3 at
         * +0x44) but still waits for its install package (+0x46, from +0x3C): clear that wait, as a
         * PS4 has it once the Blu-ray install finished (install_i2 loaded). */
        static int ungate = -1;
        if (ungate < 0) { const char *e = getenv("ISS_PIECE_UNGATE"); ungate = e && e[0] == '1'; }
        if (ungate && object && flags[0] == 3 && flags[2] == 1 && flags[3] == 0) {
            const int32_t zero = 0;
            const uint8_t no = 0;
            memcpy((void *)(uintptr_t)(object + 0x3c), &zero, 4);
            memcpy((void *)(uintptr_t)(object + 0x46), &no, 1);
            printf("TOD_WATCH %ds: section '%s' installed but waiting for its install package: wait cleared (ISS_PIECE_UNGATE)\n",
                   seconds - play, section);
            fflush(stdout);
        }
        if (force_name[0] && !forced && seconds - play >= force_after) {
            forced = 1;
            const uint64_t id = tw_find(force_name);
            if (id) {
                memcpy((void *)(uintptr_t)(s + 0x10), &id, 8);
                printf("TOD_WATCH: asked the game for '%s' (%#llx) after %ds (ISS_TOD_FORCE)\n", force_name, (unsigned long long)id, force_after);
            } else printf("TOD_WATCH: '%s' is not in the time-of-day table (ISS_TOD_FORCE)\n", force_name);
            fflush(stdout);
        }
    }
    return 0;
}
/* ISS_EXPOSURE_WATCH=1 (diagnostics, 2026-11-20: the bright flash after unpausing at ultrawide):
 * the exposure meter's tile results the CPU sums (eboot 0x171610: buffer descriptor
 * *(image+0xE5E6A8+0x260), address ([+8]&0xfff)<<32 | [+4]; frames since reset at +0x218). For 4 s
 * after each unpause (pause counter image+0x1D1F7FC back to 0), ten times a second: the buffer's
 * page split, and the sum and zero count of the tiles on each page (540 tiles at 3440x1440). */
static DWORD WINAPI ew2_thread(LPVOID unused) {
    (void)unused;
    const char *n = getenv("ISS_EXPOSURE_TILES");
    const int tiles = n && atoi(n) > 0 && atoi(n) <= 1024 ? atoi(n) : 540;
    uint32_t last_pause = 0;
    int left = 0;
    for (;; Sleep(100)) {
        const uint32_t pause = tw_u32((uintptr_t)tw_image + 0x1D1F7FC);
        if (last_pause && !pause) { left = 40; printf("EXPOSURE_WATCH: unpaused\n"); }
        if (!last_pause && pause) { left = 10; printf("EXPOSURE_WATCH: paused\n"); }
        last_pause = pause;
        if (left <= 0) continue;
        --left;
        const uint64_t g = (uintptr_t)tw_image + 0xE5E6A8;
        const uint64_t desc = tw_u64(g + 0x260);
        const uint32_t frames = tw_u32(g + 0x218);
        const uint64_t addr = desc ? (((uint64_t)(tw_u32(desc + 8) & 0xfff)) << 32) | tw_u32(desc + 4) : 0;
        if (!addr) { printf("EXPOSURE_WATCH: no buffer (frames %u)\n", frames); continue; }
        static float v[1024];
        if (!tw_read(addr, v, (size_t)tiles * 4)) { printf("EXPOSURE_WATCH: unreadable %#llx\n", (unsigned long long)addr); continue; }
        const int split = (int)((4096 - (addr & 4095)) / 4);  /* tiles on the first page */
        double s1 = 0, s2 = 0; int z1 = 0, z2 = 0, bad = 0;
        for (int i = 0; i < tiles; ++i) {
            const float x = v[i];
            if (x != x || x > 1e30f || x < -1e30f) { ++bad; continue; }
            if (i < split) { s1 += x; z1 += x == 0.f; } else { s2 += x; z2 += x == 0.f; }
        }
        printf("EXPOSURE_WATCH: buf %#llx (page offset %#llx, %d of %d tiles on the first page) frames %u | page1 sum %.4g zeros %d | page2 sum %.4g zeros %d | bad %d | avg %.5g\n",
               (unsigned long long)addr, (unsigned long long)(addr & 4095), split < tiles ? split : tiles, tiles, frames,
               s1, z1, s2, z2, bad, (s1 + s2) / tiles);
        fflush(stdout);
    }
    return 0;
}
static void iss_tod_watch_start(unsigned char *image_base) {
    const char *ew = getenv("ISS_EXPOSURE_WATCH");
    if (ew && !strcmp(ew, "1")) {
        tw_image = image_base;
        HANDLE t2 = CreateThread(NULL, 0, ew2_thread, NULL, 0, NULL);
        if (t2) CloseHandle(t2);
        printf("EXPOSURE_WATCH: on\n");
    }
    const char *on = getenv("ISS_TOD_WATCH");
    if (!on || strcmp(on, "1")) return;
    tw_image = image_base;
    HANDLE t = CreateThread(NULL, 0, tw_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    printf("TOD_WATCH: on (image %p)\n", (void *)image_base);
}
#else
static void iss_tod_watch_start(unsigned char *image_base) { (void)image_base; }
#endif
#endif
