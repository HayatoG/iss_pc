/* Exercise ISS_KBD_PROMPTS_ATLAS runtime overlay in runtime_file.c (read & pread). */
#include "../src/runtime_file.c"
#include <assert.h>
#ifdef _WIN32
#include "../src/compat_win.c"
#endif

static int32_t guest_errno;
int32_t *runtime_errno(void) { return &guest_errno; }
int32_t runtime_guest_errno(int e) { return e; }
void runtime_wait_begin(const char *kind, const void *object, const char *name, const void *const *owner, uintptr_t frame) {
    (void)kind; (void)object; (void)name; (void)owner; (void)frame;
}
void runtime_wait_end(void) {}
void tlggpu_note_open(const char *guest_path) { (void)guest_path; }
void runtime_karma_observe_save_read(void) {}
void iss_karma_save_open(int fd, int writing, uint64_t length) { (void)fd; (void)writing; (void)length; }
void iss_karma_save_part(int fd, const void *bytes, int64_t offset, int64_t size, int writing) {
    (void)fd; (void)bytes; (void)offset; (void)size; (void)writing;
}
void iss_karma_save_close(int fd) { (void)fd; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t n, const char *name) {
    for (size_t i = 0; i < n; ++i) if (!strcmp(table[i].name, name)) return (uintptr_t)table[i].function;
    return 0;
}

static void write_file_bytes(const char *path, const unsigned char *data, size_t len) {
    FILE *fp = fopen(path, "wb");
    assert(fp);
    assert(fwrite(data, 1, len, fp) == len);
    assert(!fclose(fp));
}

int main(void) {
    char root[512];
#ifdef _WIN32
    snprintf(root, sizeof(root), "%s\\iss-kbd-test-%lu", getenv("TEMP") ? getenv("TEMP") : ".", (unsigned long)GetCurrentProcessId());
    mkdir(root, 0755);
#else
    snprintf(root, sizeof(root), "/tmp/iss-kbd-test-%ld", (long)getpid());
    mkdir(root, 0755);
#endif
    char game[512], art[512], cache[512], user[512], lang[512], atlas_path[512];
    snprintf(game, sizeof(game), "%s/game", root);
    snprintf(art, sizeof(art), "%s/game/art", root);
    snprintf(cache, sizeof(cache), "%s/game/art/cache", root);
    snprintf(user, sizeof(user), "%s/user", root);
    snprintf(lang, sizeof(lang), "%s/lang_english_text.xpps", cache);
    snprintf(atlas_path, sizeof(atlas_path), "%s/kbd_atlas.bin", root);
    mkdir(game, 0755);
    mkdir(art, 0755);
    mkdir(cache, 0755);

    /* Build synthetic lang_english_text.xpps with atlas at 0x208 + 0x400 = 0x608 and 64-byte trailer. */
    const size_t atlas_off = 0x608;
    const size_t total_len = atlas_off + KBD_ATLAS_BYTES + 64;
    unsigned char *xpps = calloc(1, total_len);
    unsigned char *patched = malloc(KBD_ATLAS_BYTES);
    assert(xpps && patched);
    memcpy(xpps, "KCAP", 4);
    uint32_t dw0 = 0x400u, dw1 = 0x00a00000u, dw2 = 0x007fc1ffu;
    memcpy(xpps + 0x4f0, &dw0, 4);
    memcpy(xpps + 0x4f4, &dw1, 4);
    memcpy(xpps + 0x4f8, &dw2, 4);
    const unsigned char oid[8] = {0x29, 0xc1, 0x1c, 0x0c, 0xac, 0x5b, 0x95, 0xed};
    memcpy(xpps + 0x510, oid, 8);
    memset(xpps + atlas_off, 0x11, KBD_ATLAS_BYTES);
    memset(xpps + atlas_off + KBD_ATLAS_BYTES, 0x77, 64);
    for (size_t i = 0; i < KBD_ATLAS_BYTES; ++i) patched[i] = (unsigned char)((i * 13 + 0xA5) & 0xFF);

    write_file_bytes(lang, xpps, total_len);
    write_file_bytes(atlas_path, patched, KBD_ATLAS_BYTES);

    runtime_file_configure(game, user);

#ifdef _WIN32
    _putenv_s("ISS_KBD_PROMPTS_ATLAS", atlas_path);
#else
    setenv("ISS_KBD_PROMPTS_ATLAS", atlas_path, 1);
#endif

    int fd = (int)do_open("/app0/art/cache/lang_english_text.xpps", 0, 0);
    assert(fd >= 3);

    /* 1. pread straddling start of atlas (16 bytes before + 32 bytes inside). */
    unsigned char buf[128];
    assert(do_pread(fd, buf, 48, (int64_t)(atlas_off - 16)) == 48);
    for (int i = 0; i < 16; ++i) assert(buf[i] == 0x00);
    for (int i = 0; i < 32; ++i) assert(buf[16 + i] == patched[i]);

    /* 2. pread straddling end of atlas (32 bytes inside + 16 bytes trailer). */
    assert(do_pread(fd, buf, 48, (int64_t)(atlas_off + KBD_ATLAS_BYTES - 32)) == 48);
    for (int i = 0; i < 32; ++i) assert(buf[i] == patched[KBD_ATLAS_BYTES - 32 + i]);
    for (int i = 0; i < 16; ++i) assert(buf[32 + i] == 0x77);

    /* 3. sequential lseek + read inside the middle of the atlas. */
    const size_t mid = 123456;
    assert(do_lseek(fd, (int64_t)(atlas_off + mid), 0) == (int64_t)(atlas_off + mid));
    assert(do_read(fd, buf, 64) == 64);
    assert(memcmp(buf, patched + mid, 64) == 0);

    assert(!do_close(fd));
    free(xpps);
    free(patched);
    unlink(lang);
    unlink(atlas_path);
    puts("Runtime keyboard prompts atlas overlay: pread/read boundary tests PASS");
    return 0;
}
