/* SPDX-License-Identifier: GPL-2.0-or-later
 * Independently verified CUSA00223 01.07 main-camera update and aspect setter.
 * Does not hook input, alter camera angles, or change vertical FOV. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
enum { ISS_NATIVE_ASPECT_SITE=0x472925,ISS_NATIVE_ASPECT_STOLEN=21 };
static const unsigned char iss_native_aspect_site[]={0x83,0xbf,0x94,6,0,0,0,0x89,0xf0,0x0f,0x95,0xc2,0xc1,0xe8,8,0x40,8,0xc6,0x0f,0x95,0xc3};
static const unsigned char iss_native_main_load[]={0x48,0x8d,5,0xd0,0xe8,0xa9,7,0x48,0x8b,0xb8,0xd8,0xc0,1,0};
static const unsigned char iss_native_aspect_setter[]={0x55,0x48,0x89,0xe5,0x53,0x50,0x48,0x89,0xfb,0xc5,0xfa,0x11,0x83,0x9c,5,0,0,0x48,0x8b,3,0xff,0x90,0x20,1,0,0,0x48,0x8b,3,0x48,0x89,0xdf,0x48,0x83,0xc4,8,0x5b,0x5d,0xff,0xa0,0x28,1,0,0};
static const unsigned char iss_native_aspect_projection[]={0xc5,0xe8,0x57,0xd2,0xc5,0xf8,0x10,0x35,0x24,0x29,0x56,0};
static const unsigned char iss_native_aspect_frustum[]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x48,0x83,0xec,0x58};
/* inFAMOUS First Light 1.04 (2026-11-12): the same main-camera update and aspect setter. The site reads
 * the player object's field at +0x67c (Second Son +0x694); the camera load and the projection's constant
 * load carry their own rip displacements; the frustum and setter bytes are identical. */
static const unsigned char iss_native_aspect_site_fl[]={0x83,0xbf,0x7c,6,0,0,0,0x89,0xf0,0x0f,0x95,0xc2,0xc1,0xe8,8,0x40,8,0xc6,0x0f,0x95,0xc3};
static const unsigned char iss_native_main_load_fl[]={0x48,0x8d,5,0x79,0xb5,0xc6,7,0x48,0x8b,0xb8,0x38,0xc3,1,0};
static const unsigned char iss_native_aspect_projection_fl[]={0xc5,0xe8,0x57,0xd2,0xc5,0xf8,0x10,0x35,0xf4,0x6b,0x57,0};
typedef struct {
 uint32_t site,main_load,projection,frustum,setter,main_camera;
 const unsigned char *site_bytes,*main_load_bytes,*projection_bytes;
} IssNativeAspectLayout;
static const IssNativeAspectLayout iss_native_aspect_layout_ss={ISS_NATIVE_ASPECT_SITE,0x472909,0x581c90,0x5821b0,0x581b70,0x7f2d2b8,
 iss_native_aspect_site,iss_native_main_load,iss_native_aspect_projection};
static const IssNativeAspectLayout iss_native_aspect_layout_fl={0x51c95c,0x51c940,0x65c470,0x65c990,0x65c350,0x81a41f8,
 iss_native_aspect_site_fl,iss_native_main_load_fl,iss_native_aspect_projection_fl};
static inline int iss_native_aspect_signature_for(const IssNativeAspectLayout *L,const unsigned char* image,size_t size){
 return L && image && size>=L->frustum+sizeof(iss_native_aspect_frustum) && size>=L->setter+sizeof(iss_native_aspect_setter) &&
  size>=L->site+sizeof(iss_native_aspect_site) && size>=L->main_load+sizeof(iss_native_main_load) &&
  !memcmp(image+L->site,L->site_bytes,sizeof(iss_native_aspect_site)) &&
  !memcmp(image+L->main_load,L->main_load_bytes,sizeof(iss_native_main_load)) &&
  !memcmp(image+L->projection,L->projection_bytes,sizeof(iss_native_aspect_projection)) &&
  !memcmp(image+L->frustum,iss_native_aspect_frustum,sizeof(iss_native_aspect_frustum)) &&
  !memcmp(image+L->setter,iss_native_aspect_setter,sizeof(iss_native_aspect_setter));
}
static inline int iss_native_aspect_signature(const unsigned char* image,size_t size){
 return image && size>=0x5821b0+sizeof(iss_native_aspect_frustum) &&
  !memcmp(image+ISS_NATIVE_ASPECT_SITE,iss_native_aspect_site,sizeof(iss_native_aspect_site)) &&
  !memcmp(image+0x472909,iss_native_main_load,sizeof(iss_native_main_load)) &&
  !memcmp(image+0x581c90,iss_native_aspect_projection,sizeof(iss_native_aspect_projection)) &&
  !memcmp(image+0x5821b0,iss_native_aspect_frustum,sizeof(iss_native_aspect_frustum)) &&
  !memcmp(image+0x581b70,iss_native_aspect_setter,sizeof(iss_native_aspect_setter));
}
static inline int iss_native_aspect_tangent(float old_aspect,float tan_x,float tan_y,
 uint32_t width,uint32_t height,float *new_aspect,float *new_tan_x){
 if(!new_aspect||!new_tan_x||!width||!height||!isfinite(old_aspect)||!isfinite(tan_x)||!isfinite(tan_y)||
    old_aspect<0.5f||old_aspect>4.0f||tan_y<0.01f||tan_y>10.0f||
    fabsf(tan_x-tan_y*old_aspect)>0.002f*fmaxf(1.0f,fabsf(tan_x)))return 0;
 float aspect=(float)width/(float)height;
 if(!isfinite(aspect)||aspect<0.5f||aspect>4.0f)return 0;
 *new_aspect=aspect;*new_tan_x=tan_y*aspect;return 1;
}
