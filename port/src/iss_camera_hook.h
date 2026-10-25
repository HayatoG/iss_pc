/* inFAMOUS Second Son CUSA00223 01.07: effective angular-speed limits.
 * Verified ELF SHA256 b3afd173a379f83e1e408e71fdef22b78f93092d332b79decd48a18a08bfc605.
 * No pad scaling. This hook replays two stores after zoom/mode preferences.
 * Adaptive input gain, acceleration, pitch constraints and angle integration remain in-game.
 */
#ifndef ISS_CAMERA_HOOK_H
#define ISS_CAMERA_HOOK_H
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>

/* Known European merged 1.07 dumps can retain APP_VER 01.00. Instruction signatures
 * below remain mandatory; these metadata values alone never authorize a patch. */
static inline int iss_camera_title_supported(const char *serial, const char *version) {
    return (!strcmp(serial,"CUSA00223") && !strcmp(version,"01.07")) ||
           (!strcmp(serial,"CUSA00004") && (!strcmp(version,"01.07") || !strcmp(version,"01.00")));
}

#define ISS_CAMERA_SITE 0x14d943u
#define ISS_CAMERA_LENGTH 34u
#define ISS_CAMERA_STUB_SIZE 82u
static const unsigned char iss_camera_original[ISS_CAMERA_LENGTH] = {
  0xc5,0xfa,0x11,0x84,0x24,0x04,0x01,0x00,0x00,
  0xc5,0xfa,0x10,0x44,0x24,0x30,
  0xc4,0xc1,0x7a,0x59,0x84,0x86,0x0c,0x04,0x00,0x00,
  0xc5,0xfa,0x11,0x84,0x24,0x08,0x01,0x00,0x00
};

/* Strict finite bounded options. Unset is identity; invalid values reject the whole hook. */
static inline int iss_camera_scale(const char *text, float *scale) {
    *scale = 1.0f;
    if (!text || !*text) return 1;
    char *end;
    errno = 0;
    float value = strtof(text, &end);
    if (end == text || *end || errno == ERANGE || !isfinite(value) || value < 0.25f || value > 4.0f)
        return 0;
    *scale = value;
    return 1;
}
static inline int iss_camera_bytes(const unsigned char *image, uint64_t size,
                                   uint32_t at, const unsigned char *bytes, unsigned length) {
    return at <= size && length <= size-at && !memcmp(image+at,bytes,length);
}
static inline int iss_camera_signature_match(const unsigned char *image, uint64_t size) {
    static const unsigned char entry[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xe4};
    static const unsigned char demand[] = {0xc4,0xc1,0x7a,0x59,0x54,0x24,0x04,0xc5,0xe2,0x59,0xc1,0xc4,0xc1,0x7a,0x59,0x44,0x24,0x08};
    static const unsigned char angles[] = {0xc5,0xfa,0x7f,0x00,0xc5,0xf8,0x28,0x84,0x24,0x80,0x00,0x00,0x00};
    return iss_camera_bytes(image,size,ISS_CAMERA_SITE,iss_camera_original,ISS_CAMERA_LENGTH) &&
           iss_camera_bytes(image,size,0x14d6f0,entry,sizeof(entry)) &&
           iss_camera_bytes(image,size,0x14b01e,demand,sizeof(demand)) &&
           iss_camera_bytes(image,size,0x14dc43,angles,sizeof(angles));
}

/* VEX vmulss preserves XMM0 upper lanes like the original scalar computation.
 * No stack movement, GP register, flags, calls or absolute data pointers.
 * An indirect absolute tail jump avoids rel32 allocation constraints.
 * Layout: multiply H, original store H, original load/multiply V, multiply V,
 * original store V, absolute jump back, two float constants.
 */
static inline unsigned iss_camera_build_stub(unsigned char *out, uintptr_t resume,
                                              float horizontal, float vertical) {
    static const unsigned char mul[] = {0xc5,0xfa,0x59,0x05,0,0,0,0};
    static const unsigned char jump[] = {0xff,0x25,0,0,0,0};
    int32_t disp = 66; /* constant H at 74, next instruction at 8 */
    memcpy(out,mul,8); memcpy(out+4,&disp,4);
    memcpy(out+8,iss_camera_original,25); /* H store, V load, V preference multiply */
    memcpy(out+33,mul,8); disp = 37; /* constant V at 78, next instruction at 41 */
    memcpy(out+37,&disp,4);
    memcpy(out+41,iss_camera_original+25,9);
    memcpy(out+50,jump,6); memcpy(out+56,&resume,8);
    /* Constants follow the jump's eight-byte target; padded to 74 for stable encoding. */
    memset(out+64,0x90,10);
    memcpy(out+74,&horizontal,4); memcpy(out+78,&vertical,4);
    return ISS_CAMERA_STUB_SIZE;
}
static inline void iss_camera_build_jump(unsigned char *out, uintptr_t stub) {
    static const unsigned char jump[] = {0xff,0x25,0,0,0,0};
    memcpy(out,jump,6); memcpy(out+6,&stub,8);
    memset(out+14,0x90,ISS_CAMERA_LENGTH-14);
}
#endif
