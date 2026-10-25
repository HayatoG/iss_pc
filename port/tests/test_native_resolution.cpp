// SPDX-License-Identifier: GPL-2.0-or-later
#include "../src/iss_native_resolution.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include <algorithm>
static uint32_t read32(const unsigned char* p){uint32_t x;memcpy(&x,p,4);return x;}
static uint64_t read64(const unsigned char* p){uint64_t x;memcpy(&x,p,8);return x;}
static void fixture(std::vector<unsigned char>& image){
    image.assign(0x600000,0xcc);
    for(const auto& s:iss_native_sites)memcpy(image.data()+s.offset,s.before,s.length);
    memcpy(image.data()+0x58030c,iss_native_projection,sizeof(iss_native_projection));
    memcpy(image.data()+0x5802e6,iss_native_aspect_width,sizeof(iss_native_aspect_width));
}
int main(int argc,char** argv){
    IssNativePlan p;
    for(int output=1;output<ISS_NATIVE_PRESETS;output++){
        assert(iss_native_plan({output,100,1},&p));
        assert(p.width==iss_native_widths[output] && p.height==iss_native_heights[output]);
        assert(p.ui_height==1080);
        const double aspect=double(p.output_width)/p.output_height;
        assert(std::abs(double(p.ui_width)/p.ui_height-aspect)<0.001);
        // Hor+ geometry: a fixed vertical half-FOV grows horizontally with aspect.
        double vfov=60.0*3.141592653589793/180;
        double hfov=2*std::atan(std::tan(vfov/2)*aspect);
        assert(std::abs(2*std::atan(std::tan(hfov/2)/aspect)-vfov)<1e-12);
        const uint64_t needed=uint64_t((p.width+15)/16)*((p.height+15)/16)*128;
        assert(p.light_entries>=needed && p.light_entries>=0x320000 && p.light_entries%65536==0);
        for(int scale=25;scale<=200;scale++){
            bool ok=iss_native_plan({output,scale,1},&p);
            if(ok){assert(!(p.width%2) && !(p.height%2));assert(uint64_t(p.width)*p.height<=8294400);}
            else assert(!p.width && !p.height);
        }
    }
    assert(iss_native_plan({3,100,1},&p) && p.width==3440 && p.height==1440 && p.ui_width==2580 && p.light_entries==0x320000);
    assert(iss_native_plan({5,100,1},&p) && p.width==5120 && p.height==1440 && p.ui_width==3840 && p.light_entries==3735552);
    assert(iss_native_plan({4,100,1},&p) && p.light_entries==4194304);
    assert(iss_native_plan({3,50,1},&p) && p.width==1720 && p.height==720);
    for(auto c:{IssNativeConfig{0,100,1},{6,100,1},{3,0,1},{3,201,1},{3,100,2},{5,200,1}})assert(!iss_native_plan(c,&p));
    FILE* f=tmpfile();assert(f);fputs("iss_native_output=5\niss_native_percent=100\niss_native_hud_edges=0\n",f);rewind(f);
    IssNativeConfig c;assert(iss_native_config(f,&c) && c.output==5 && c.percent==100 && c.hud_edges==0);fclose(f);
    f=tmpfile();assert(f);fputs("iss_native_output=3\niss_native_percent=100oops\n",f);rewind(f);assert(!iss_native_config(f,&c));fclose(f);
    std::vector<unsigned char> image;fixture(image);const auto original=image;
    // Each instruction byte is independently corrupted. Even the last lighting
    // descriptor must refuse before a single dimension or HUD byte is changed.
    unsigned guards=0;
    for(const auto& s:iss_native_sites)for(unsigned byte=0;byte<s.length;byte++){
        image=original;image[s.offset+byte]^=1;auto before=image;
        assert(!iss_native_apply(image.data(),image.size(),"CUSA00223","01.07",{5,100,1}));assert(image==before);guards++;
    }
    for(unsigned byte=0;byte<sizeof(iss_native_projection);byte++){
        image=original;image[0x58030c+byte]^=1;auto before=image;
        assert(!iss_native_apply(image.data(),image.size(),"CUSA00223","01.07",{3,100,1}));assert(image==before);guards++;
    }
    for(unsigned byte=0;byte<sizeof(iss_native_aspect_width);byte++){
        image=original;image[0x5802e6+byte]^=1;auto before=image;
        assert(!iss_native_apply(image.data(),image.size(),"CUSA00223","01.07",{3,100,1}));assert(image==before);guards++;
    }
    image=original;
    assert(!iss_native_apply(image.data(),image.size(),"CUSA00004","01.07",{3,100,1}));assert(image==original);
    assert(!iss_native_apply(image.data(),image.size(),"CUSA00223","01.00",{3,100,1}));assert(image==original);
    assert(!iss_native_apply(image.data(),100,"CUSA00223","01.07",{3,100,1}));assert(image==original);
    assert(!iss_native_apply(image.data(),image.size(),"CUSA00223","01.07",{5,200,1}));assert(image==original);
    assert(iss_native_apply(image.data(),image.size(),"CUSA00223","01.07",{5,100,1}));
    assert(read32(image.data()+0x1eff4a)==5120 && read32(image.data()+0x1eff51)==1440);
    assert(read32(image.data()+0x1f0403)==3840 && read32(image.data()+0x1f03fb)==1080);
    assert(read32(image.data()+0x16be07)==3735552 && read32(image.data()+0x16be1f)==3735552);
    // Check every byte outside the patch plan, including the camera/karma sites.
    auto expected=original;IssNativeSite changes[ISS_NATIVE_SITE_COUNT];assert(iss_native_plan({5,100,1},&p));iss_native_changes(&p,changes);
    for(const auto& s:changes)memcpy(expected.data()+s.offset,s.after,s.length);assert(image==expected);
    runtime_native_publish(-1,"Refused",nullptr);assert(runtime_native_status()==-1 && runtime_native_safe_area()==1.0f);
    p.hud_edges=0;runtime_native_publish(1,"Active",&p);assert(runtime_native_safe_area()==0.5f);
    p.hud_edges=1;runtime_native_publish(1,"Active",&p);assert(runtime_native_safe_area()==1.0f);
    // Separating the UI canvas must not narrow native world targets or its
    // camera aspect. Verify actual RIP-relative pointer destinations.
    for(int output:{3,5}){
        assert(iss_native_plan({output,100,0,1},&p));
        assert(p.ui_width==1920&&p.ui_height==1080&&p.width==iss_native_widths[output]&&p.height==1440);
        image=original;assert(iss_native_apply(image.data(),image.size(),"CUSA00223","01.07",{output,100,0,1}));
        const int64_t world_width=0x5802e6+7+int64_t(int32_t(read32(image.data()+0x5802e6+3)));
        const int64_t world_height=0x58030c+7+int64_t(int32_t(read32(image.data()+0x58030c+3)));
        assert(world_width==0x1eff4a&&world_height==0x1eff51);
        assert(read32(image.data()+world_width)==p.width&&read32(image.data()+world_height)==p.height);
        assert(read32(image.data()+0x1f0403)==1920&&read32(image.data()+0x1f03fb)==1080);
    }
    f=tmpfile();assert(f);fputs("iss_native_ui_16_9=1\n",f);rewind(f);assert(iss_native_config(f,&c)&&c.ui_16_9==1);fclose(f);
    for(int output:{1,2,3,4,5}){
        assert(iss_native_plan({output,100,0,2},&p));
        assert(p.ui_width*9==p.ui_height*16);
        assert(p.width==iss_native_widths[output]&&p.height==iss_native_heights[output]);
        if(output==3)assert(p.ui_width==2592&&p.ui_height==1458);
        if(output==5)assert(p.ui_width==3840&&p.ui_height==2160);
        image=original;assert(iss_native_apply(image.data(),image.size(),"CUSA00223","01.07",{output,100,0,2}));
        assert(read32(image.data()+0x1f0403)==p.ui_width&&read32(image.data()+0x1f03fb)==p.ui_height);
    }
    f=tmpfile();assert(f);fputs("iss_native_ui_16_9=2\n",f);rewind(f);assert(iss_native_config(f,&c)&&c.ui_16_9==2);fclose(f);
    f=tmpfile();assert(f);fputs("iss_native_ui_16_9=3\n",f);rewind(f);assert(!iss_native_config(f,&c));fclose(f);
    // Optional local-only executable validation: parse PT_LOAD segments instead
    // of assuming file offsets equal virtual addresses. No executable is shipped.
    if(argc>1){
        std::ifstream stream(argv[1],std::ios::binary);assert(stream);
        std::vector<unsigned char> elf((std::istreambuf_iterator<char>(stream)),{});
        assert(elf.size()>64 && !memcmp(elf.data(),"\x7f" "ELF",4) && elf[4]==2);
        std::vector<unsigned char> mapped(0x600000,0);
        uint64_t phoff=read64(elf.data()+32);uint16_t stride,n;memcpy(&stride,elf.data()+54,2);memcpy(&n,elf.data()+56,2);
        for(unsigned i=0;i<n;i++){
            auto at=phoff+uint64_t(i)*stride;assert(at+56<=elf.size());auto h=elf.data()+at;
            if(read32(h)!=1)continue;
            uint64_t off=read64(h+8),va=read64(h+16),size=read64(h+32);
            assert(off+size<=elf.size());if(va>=mapped.size())continue;
            size=std::min<uint64_t>(size,mapped.size()-va);memcpy(mapped.data()+va,elf.data()+off,size);
        }
        for(int output=1;output<ISS_NATIVE_PRESETS;output++){
            auto copy=mapped;assert(iss_native_apply(copy.data(),copy.size(),"CUSA00223","01.07",{output,100,1}));
        }
        puts("Actual US ELF: all guarded patch plans accepted (no disk writes).");
    }
    printf("Native resolution: exact dimensions, scale bounds, Hor+ math, lighting capacity, %u corrupted signature bytes, atomic refusal and safe-area fallback passed.\n",guards);
}
