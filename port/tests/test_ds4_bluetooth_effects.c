#undef NDEBUG
#include <assert.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_hidapi.h>
static unsigned writes;
static unsigned char received[462];
static size_t received_size;
static int capture_write(SDL_hid_device *device,const unsigned char *data,size_t size) {
    (void)device;
    assert(size<=sizeof(received));
    SDL_memcpy(received,data,size);
    received_size=size;
    ++writes;
    return (int)size;
}
#define SDL_hid_write capture_write
#include "../src/controller_ds4_bluetooth.h"

int main(void) {
    Ds4BluetoothSpeaker speaker={0};
    speaker.effects_lock=SDL_CreateMutex();
    assert(speaker.effects_lock);
    assert(ds4_bt_effects(&speaker) && !writes);
    speaker.low_motor=17;
    speaker.high_motor=29;
    speaker.rgb[0]=33;
    speaker.rgb[1]=77;
    speaker.rgb[2]=99;
    speaker.effects_dirty=3;
    speaker.rumble_until=SDL_GetTicksNS()+1000000000;
    assert(ds4_bt_effects(&speaker));
    assert(writes==1 && received_size==78);
    assert(received[0]==0x11 && received[2]==0xa0 && received[3]==0x83);
    assert(received[6]==29 && received[7]==17 && received[24]==0x4f);
    assert(!SDL_memcmp(received+8,speaker.rgb,3));
    const Uint8 prefix=0xa2;
    Uint32 crc=SDL_crc32(SDL_crc32(0,&prefix,1),received,74),actual;
    SDL_memcpy(&actual,received+74,4);
    assert(crc==actual);
    assert(ds4_bt_effects(&speaker) && writes==1);
    speaker.rumble_until=1;
    assert(ds4_bt_effects(&speaker) && writes==2);
    assert(received[2]==0xa0 && received[3]==0x81);
    assert(!received[6] && !received[7]); /* timed motor stop keeps speaker active */
    assert(sbc_init(&speaker.codec,0)==0);
    speaker.codec.frequency=SBC_FREQ_32000;
    speaker.codec.blocks=SBC_BLK_16;
    speaker.codec.subbands=SBC_SB_8;
    speaker.codec.mode=SBC_MODE_JOINT_STEREO;
    speaker.codec.allocation=SBC_AM_SNR;
    speaker.codec.bitpool=48;
    speaker.codec.endian=SBC_LE;
    const Uint8 pcm[DS4_BT_PCM_BYTES]={0};
    assert(ds4_bt_send(&speaker,pcm));
    assert(received_size==462 && received[0]==0x17 && received[2]==0xa0);
    assert(speaker.sequence==4);
    for (int i=0;i<4;++i) assert(received[6+109*i]==0x9c);
    sbc_finish(&speaker.codec);
    SDL_DestroyMutex(speaker.effects_lock);
    puts("PASS: combined Bluetooth effects preserve audio, expire motors, suppress duplicates, and frame SBC correctly");
}
