/* System functions only inFAMOUS Second Son (CUSA00223 1.07) imports. All describe an offline
 * console with the whole game installed: PSN signed out (the "A" NpManager variants take a user
 * id instead of an online id, same answers as runtime_services.c), PlayGo with nothing left to
 * download, networking calls that succeed without a network (add-ons: runtime_content.c). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "runtime.h"

#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define SYSTEM_SERVICE_INVALID ((int32_t)0x80a10003)

static void once(const char *what) { printf("Runtime: %s (inFAMOUS)\n", what); }

/* sceSystemServiceParamGetString(id, buffer, size): only the user name parameter exists. */
static ABI int32_t param_string(int32_t id, char *buffer, uint64_t size) {
    if (!buffer || !size) return SYSTEM_SERVICE_INVALID;
    snprintf(buffer, size, "%s", "Player");
    printf("Runtime: sceSystemServiceParamGetString(%d) -> Player\n", id);
    return 0;
}
static ABI int32_t ok(void) { return 0; }
static ABI int32_t signed_out(void) { return NP_SIGNED_OUT; }
static ABI int32_t np_register_a(void *callback, void *arg) { (void)callback; (void)arg; static int32_t next = 0x100; return ++next; }
static ABI int32_t netctl_init(void) { once("sceNetCtlInit"); return 0; }
/* scePlayGoGetEta(handle, chunks, count, out_eta): everything is installed. */
static ABI int32_t playgo_eta(int32_t handle, const uint16_t *chunks, uint32_t count, uint64_t *eta) {
    (void)handle; (void)chunks; (void)count;
    if (eta) *eta = 0;
    return 0;
}
/* scePlayGoGetToDoList(handle, list, count, out_entries): no pending chunk. */
static ABI int32_t playgo_todo(int32_t handle, void *list, uint32_t count, uint32_t *entries) {
    (void)handle; (void)list; (void)count;
    if (entries) *entries = 0;
    return 0;
}
static ABI int32_t playgo_language_mask(int32_t handle, uint64_t mask) {
    printf("Runtime: scePlayGoSetLanguageMask(%#llx)\n", (unsigned long long)mask);
    (void)handle;
    return 0;
}

static const RuntimeExport exports[] = {
    {"sceSystemServiceParamGetString", param_string},
    {"sceHttpSetChunkedTransferEnabled", ok}, {"sceHttpsLoadCert", ok},
    {"sceNetCtlInit", netctl_init}, {"sceNetCtlTerm", ok},
    {"scePlayGoGetEta", playgo_eta}, {"scePlayGoGetToDoList", playgo_todo},
    {"scePlayGoSetLanguageMask", playgo_language_mask},
    {"sceNpCheckNpAvailabilityA", signed_out}, {"sceNpGetAccountCountryA", signed_out},
    {"sceNpGetAccountIdA", signed_out}, {"sceNpGetGamePresenceStatusA", signed_out},
    {"sceNpRegisterGamePresenceCallbackA", np_register_a}, {"sceNpRegisterStateCallbackA", np_register_a},
    /* sceAppContentGetAddcontInfo: src/runtime_content.c (the profile's installed add-ons) */
};
uintptr_t runtime_infamous_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }
