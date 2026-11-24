// SPDX-License-Identifier: GPL-2.0-or-later
#include "tlg_trophies.h"
#include "tlg_png.h"
#ifndef ISS_TROPHY_BACKEND_TEST
#include "imgui_internal.h"
#include "overlay_design.h"
#endif
#include "../../src/iss_trophies.h"
#include "../../src/trophy_store.h"
#include "../../src/trophy_save_session.h"
#include "../../src/trophy_catalogue.h"
#include <mutex>
#include <fstream>
#include <algorithm>
#include <ctime>
#include <cstring>
#include <deque>
#include <chrono>
#include <thread>
#include <cmath>
#ifndef ISS_TROPHY_BACKEND_TEST
#include <SDL3/SDL.h>
#endif
extern "C" int runtime_file_translate(const char*,char*,size_t);
extern "C" const char *runtime_file_user_dir(void);
namespace {
// Pop-ups waiting to be shown (game thread adds, render thread shows them one after another).
struct Pop { int id; double start; };
std::mutex popLock;
std::deque<Pop> pops;
bool EnvOff(const char* name){const char* v=std::getenv(name);return v&&v[0]=='0';}
void QueuePop(int id){
    if(EnvOff("ISS_TROPHY_POPUP"))return;
    std::lock_guard guard(popLock);if(pops.size()<64)pops.push_back({id,-1.0});
}
constexpr int32_t Invalid=int32_t(0x80551604u),StorageError=int32_t(0x80551607u);
std::mutex lock;
bool attempted=false,ready=false;
bool perSave=false;
bool collectibleRecovery=false;
IssTrophies::SaveSession saveSession;
std::string problem;
// Which game's trophies: Second Son (US and European: 48, ledger CUSA00223) or First Light (25).
bool firstLight=false;
int total=48;
std::string title="CUSA00223";
std::vector<std::vector<unsigned char>> pngs(48);
#ifndef ISS_TROPHY_BACKEND_TEST
std::vector<ImTextureData*> textures(48,nullptr);
#endif
// TITLE_ID of the running game's sce_sys/param.sfo ("" when unreadable).
std::string GameTitle(){
    char path[2048];if(runtime_file_translate("/app0/sce_sys/param.sfo",path,sizeof(path)))return {};
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file||file.tellg()<20||file.tellg()>1024*1024)return {};
    std::vector<unsigned char> b(size_t(file.tellg()));file.seekg(0);file.read(reinterpret_cast<char*>(b.data()),b.size());
    if(!file||std::memcmp(b.data(),"\0PSF",4))return {};
    auto le=[&](size_t p,int n){uint32_t v=0;for(int i=n-1;i>=0;i--)v=(v<<8)|b[p+i];return v;};
    uint32_t keys=le(8,4),values=le(12,4),count=le(16,4);
    if(count>256||20+size_t(count)*16>b.size()||keys>=b.size()||values>=b.size())return {};
    for(uint32_t i=0;i<count;i++){
        size_t e=20+size_t(i)*16;size_t key=keys+le(e,2);uint32_t size=le(e+4,4),offset=le(e+12,4);
        if(key>=b.size())return {};
        auto end=std::find(b.begin()+key,b.end(),0);
        if(std::string(b.begin()+key,end)!="TITLE_ID")continue;
        if(values+size_t(offset)+size>b.size())return {};
        std::string v(reinterpret_cast<const char*>(b.data()+values+offset),size);
        return v.substr(0,v.find('\0'));
    }
    return {};
}
IssTrophies::Progress progress;
std::filesystem::path ledger;
// All saves count together (per-save mode): every trophy earned or recovered in any save version
// is kept in all-saves.json at once, like a console account. The per-save ledgers are unchanged.
// If that file is unreadable, nothing is overwritten and tracking stays per save as before.
IssTrophies::Progress global;
std::filesystem::path globalPath;
bool globalOk=false;
bool GlobalAdd(int id,int64_t time,bool recovered){
    if(!globalOk||id<0||id>=48||time<=0||!global.earned.emplace(id,time).second)return false;
    if(recovered)global.recovered.insert(id);
    return true;
}
// The console, not the game, awards the platinum (ID 0) once every other trophy is earned.
bool GlobalPlatinum(){
    if(!globalOk||global.earned.contains(0))return false;
    for(int id=1;id<48;id++)if(!global.earned.contains(id))return false;
    global.earned[0]=int64_t(std::time(nullptr));
    std::puts("Runtime: trophy 0 (platinum) unlocked: every other trophy is earned across your saves");
    return true;
}
void GlobalCommit(){
    try{IssTrophies::Save(globalPath,global);}
    catch(const std::exception& e){problem=e.what();std::fprintf(stderr,"Trophies: all-saves record not written: %s\n",e.what());}
}
bool GlobalMerge(const IssTrophies::Progress& p){
    bool changed=false;
    for(auto [id,time]:p.earned)changed|=GlobalAdd(id,time,p.recovered.contains(id));
    return changed;
}
void GlobalInit(const std::string& fingerprint){
    globalPath=ledger.parent_path()/"all-saves.json";
    try{global=IssTrophies::Load(globalPath,fingerprint,48);globalOk=true;}
    catch(const std::exception& e){std::fprintf(stderr,"Trophies: all-saves record unreadable (%s); tracking stays per save\n",e.what());return;}
    size_t sources=0,unreadable=0;bool changed=false;
    std::error_code error;
    for(std::filesystem::directory_iterator it(ledger.parent_path()/"save-versions",error),end;!error&&it!=end;it.increment(error)){
        auto path=it->path();auto key=path.stem().string();
        if(path.extension()!=".json"||key.size()!=64||key.find_first_not_of("0123456789abcdef")!=std::string::npos)continue;
        try{changed|=GlobalMerge(IssTrophies::Load(path,fingerprint,48,key));sources++;}
        catch(const std::exception&){unreadable++;}
    }
    // Every save slot on disk (read only), so a save that was not loaded lately still counts.
    size_t slots=0,proven=0;
    for(std::filesystem::recursive_directory_iterator it(std::filesystem::path(runtime_file_user_dir())/"savedata",error),end;
        !error&&it!=end;it.increment(error)){
        if(it.depth()>4||it->path().filename()!="dis.dat"||!it->is_regular_file(error)||it->file_size(error)!=131072)continue;
        std::ifstream file(it->path(),std::ios::binary);std::vector<unsigned char> bytes(131072);
        if(!file.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))continue;
        try{
            auto ids=IssTrophies::RecoveredTrophies(bytes,collectibleRecovery);slots++;
            for(int id:ids)if(GlobalAdd(id,int64_t(std::time(nullptr)),true)){changed=true;proven++;}
        }catch(const std::exception&){}
    }
    changed|=GlobalPlatinum();
    if(changed||!std::filesystem::exists(globalPath))GlobalCommit();
    std::printf("Trophies: all saves count together: %zu earned from %zu save versions (%zu unreadable) and %zu save slots (%zu newly proven)\n",
        global.earned.size(),sources,unreadable,slots,proven);
}
#ifndef ISS_TROPHY_BACKEND_TEST
bool textureAttempted=false;
#endif
bool VerifyCollectibleData(){
    try{
        auto hash=[](const char* guest,size_t maximum){
            char path[2048];if(runtime_file_translate(guest,path,sizeof(path)))return std::string{};
            std::ifstream file(path,std::ios::binary|std::ios::ate);
            if(!file||file.tellg()<=0||uint64_t(file.tellg())>maximum)return std::string{};
            std::vector<unsigned char> bytes(size_t(file.tellg()));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
            return file?IssTrophies::Fingerprint(bytes):std::string{};
        };
        return IssTrophies::CollectibleDataMatches(hash("/app0/art/cache/world_seaport.xpps",64*1024*1024),hash("/app0/art/cache/i3.sprig.xpps",4*1024*1024));
    }catch(const std::exception&){return false;}
}
void Init(){
    if(attempted)return;attempted=true;
    try{
        char path[2048];
        if(runtime_file_translate("/app0/sce_sys/trophy/trophy00.trp",path,sizeof(path)))throw std::runtime_error("Game trophy archive is unavailable.");
        std::ifstream file(path,std::ios::binary|std::ios::ate);
        if(!file||file.tellg()<=0||file.tellg()>32*1024*1024)throw std::runtime_error("Cannot read game trophy archive.");
        std::vector<unsigned char> bytes(size_t(file.tellg()));file.seekg(0);file.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
        if(!file)throw std::runtime_error("Incomplete trophy archive read.");
        auto entries=IssTrophies::ParseTrp(bytes);
        if(GameTitle()=="CUSA00575"){   // inFAMOUS First Light: its own 25 trophies and ledger
            firstLight=true;total=25;title="CUSA00575";
            pngs.assign(size_t(total),{});
#ifndef ISS_TROPHY_BACKEND_TEST
            textures.assign(size_t(total),nullptr);
#endif
        }
        for(auto& entry:entries){
            for(int id=0;id<total;id++){
                char name[32];std::snprintf(name,sizeof(name),"TROP%03d.PNG",id);
                if(entry.name==name&&entry.flags==0){
                    if(entry.data.size()<24||std::memcmp(entry.data.data(),"\x89PNG\r\n\x1a\n",8))throw std::runtime_error("Invalid trophy icon.");
                    pngs[id]=std::move(entry.data);break;
                }
            }
        }
        for(auto& png:pngs)if(png.empty())throw std::runtime_error(firstLight?"Expected First Light's 25 trophy icons.":"Expected Second Son's 48 trophy icons.");
        ledger=std::filesystem::path(runtime_file_user_dir())/"trophies"/title/"earned.json";
        // Save-based tracking and recovery read Second Son's save format: First Light records what the game reports.
        const char* option=std::getenv("ISS_TROPHIES_PER_SAVE");perSave=!firstLight&&option&&!std::strcmp(option,"1");
        if(perSave&&!runtime_trophy_save_mode_ready())throw std::runtime_error("Save-specific trophy game-code check is not ready or did not match.");
        auto fingerprint=IssTrophies::Fingerprint(bytes);
        if(perSave){
            saveSession.Configure(ledger.parent_path(),fingerprint);saveSession.EnableRecovery(true);
            collectibleRecovery=VerifyCollectibleData();saveSession.EnableCollectibleRecovery(collectibleRecovery);
            std::printf("Trophies: collectible dataset verification %s\n",collectibleRecovery?"matched; recovery enabled":"unavailable/mismatched; collectible recovery disabled");
            GlobalInit(fingerprint);
        }
        else progress=IssTrophies::Load(ledger,fingerprint,total,"",title);
        ready=true;
        std::printf("Trophies: %s: %d local game icons loaded; %zu earned. Names remain encrypted.\n",title.c_str(),total,progress.earned.size());
        // Test only: ISS_TROPHY_POPUP_TEST=<id> shows that trophy's pop-up once, without earning it.
        if(const char* t=std::getenv("ISS_TROPHY_POPUP_TEST")){int id=std::atoi(t);if(id>=0&&id<total)QueuePop(id);}
    }catch(const std::exception& e){problem=e.what();std::fprintf(stderr,"Trophies: %s\n",problem.c_str());}
}
int32_t Unlock(int32_t id,int32_t* platinum){
    std::lock_guard guard(lock);Init();if(platinum)*platinum=-1;
    if(!ready||id<0||id>=total)return Invalid;
    if(perSave){
        auto now=int64_t(std::time(nullptr));
        if(GlobalAdd(id,now,false)){
            bool platinumNow=GlobalPlatinum();if(platinumNow&&platinum)*platinum=0;
            GlobalCommit();
            std::printf("Runtime: trophy %d earned and kept for all saves\n",id);
            QueuePop(id);if(platinumNow)QueuePop(0);
        }
        if(!saveSession.Bound())return globalOk?0:Invalid;
        if(saveSession.Current().earned.contains(id))return 0;
        if(!saveSession.Earn(id,now))return globalOk?0:Invalid;
        if(!globalOk){std::printf("Runtime: trophy %d earned for active save; save gameplay to retain it\n",id);QueuePop(id);}
        return 0;
    }
    if(progress.earned.contains(id))return 0;
    auto next=progress;next.earned[id]=int64_t(std::time(nullptr));
    // First Light: the console, not the game, awards the platinum (ID 0) once every other trophy is earned.
    bool platinumNow=false;
    if(firstLight&&!next.earned.contains(0)){
        platinumNow=true;for(int other=1;other<total;other++)if(!next.earned.contains(other)){platinumNow=false;break;}
        if(platinumNow)next.earned[0]=next.earned[id];
    }
    try{IssTrophies::Save(ledger,next);progress=std::move(next);problem.clear();}
    catch(const std::exception& e){problem=e.what();std::fprintf(stderr,"Trophies: unlock %d not committed: %s\n",id,e.what());return StorageError;}
    // Second Son here: no inferred platinum or save-completion unlocks, only game-reported IDs.
    std::printf("Runtime: trophy %d unlocked and saved locally\n",id);QueuePop(id);
    if(platinumNow){if(platinum)*platinum=0;std::puts("Runtime: trophy 0 (platinum) unlocked: every other trophy is earned");QueuePop(0);}
    return 0;
}
int32_t State(uint32_t* flags,uint32_t* count){
    if(!flags||!count)return Invalid;
    std::lock_guard guard(lock);Init();std::fill(flags,flags+4,0);*count=0;
    if(!ready)return Invalid;
    // The API count is the total number of trophies, not the earned count.
    // Second Son uses it as the bound when copying all returned flag bits.
    for(auto [id,time]:(perSave?(globalOk?global:saveSession.Current()):progress).earned)flags[id/32]|=uint32_t(1)<<(id%32);
    *count=uint32_t(total);return 0;
}
void SaveOpen(int fd,int writing,uint64_t length){std::lock_guard guard(lock);Init();if(ready&&perSave)saveSession.Open(fd,writing!=0,length);}
void SavePart(int fd,const void* data,int64_t offset,int64_t size,int writing){std::lock_guard guard(lock);if(ready&&perSave)saveSession.Part(fd,data,offset,size,writing!=0);}
void SaveClose(int fd){
    std::lock_guard guard(lock);if(!ready||!perSave)return;
    try{saveSession.Close(fd);problem=saveSession.RecoveryProblem();
        if(saveSession.Bound()&&GlobalMerge(saveSession.Current())){
            GlobalPlatinum();GlobalCommit();
            std::printf("Trophies: all-saves record now %zu earned\n",global.earned.size());
        }
        if(saveSession.Bound())std::printf("Trophies: active save version %.12s; %zu earned (%zu recovered)\n",saveSession.Current().save_identity.c_str(),saveSession.Current().earned.size(),saveSession.Current().recovered.size());}
    catch(const std::exception& e){problem=e.what();std::fprintf(stderr,"Trophies: save tracking: %s\n",e.what());}
}
void ProgressReset(){
    std::lock_guard guard(lock);Init();if(!ready||!perSave)return;
    saveSession.ProgressReset();problem.clear();std::puts("Trophies: player progression reset; prior save tracking detached");
}
const IssTrophyBackend backend{Unlock,State,SaveOpen,SavePart,SaveClose,ProgressReset};
struct Register { Register(){runtime_trophy_set_backend(&backend);} } registration;
}
#ifndef ISS_TROPHY_BACKEND_TEST
void TlgTrophiesUpdateTextures(){
    {std::lock_guard guard(lock);Init();if(!ready)return;}
    if(!textureAttempted){
        textureAttempted=true;
        for(int id=0;id<total;id++){
            TlgPng::Image image;
            if(!TlgPng::DecodeRgba(pngs[id].data(),pngs[id].size(),image)||image.width<1||image.height<1||image.width>512||image.height>512)continue;
            auto *t=IM_NEW(ImTextureData)();t->Create(ImTextureFormat_RGBA32,image.width,image.height);
            std::memcpy(t->GetPixels(),image.rgba.data(),image.rgba.size());ImGui::RegisterUserTexture(t);textures[id]=t;
        }
    }
}
namespace {
// The pop-up's sound: <data folder>/sounds/trophy.wav when the player put one there, else a short
// original chime made here (a soft rising three-note bell). Played on its own SDL stream from a
// short-lived thread, so the game's sound and the render thread never wait for it.
std::vector<float> Chime(int rate){
    const double notes[3]={987.77,1318.51,1975.53};   // B5, E6, B6
    const double starts[3]={0.0,0.11,0.22};
    const double length=1.5;size_t frames=size_t(length*rate);
    std::vector<float> pcm(frames*2,0.0f);
    for(int n=0;n<3;n++){
        for(size_t i=size_t(starts[n]*rate);i<frames;i++){
            double t=double(i)/rate-starts[n];
            double env=std::min(1.0,t/0.004)*std::exp(-t*(3.2+n*0.6));
            double w=2*3.14159265358979*notes[n]*t;
            double v=(std::sin(w)+0.35*std::sin(w*2.76)+0.12*std::sin(w*5.40))*env*0.16;
            pcm[i*2]+=float(v);pcm[i*2+1]+=float(v);
        }
    }
    return pcm;
}
void PlayTrophySound(){
    if(EnvOff("ISS_TROPHY_SOUND")||!(SDL_WasInit(SDL_INIT_AUDIO)&SDL_INIT_AUDIO))return;
    std::string custom=(std::filesystem::path(runtime_file_user_dir())/"sounds"/"trophy.wav").string();
    std::thread([custom]{
        SDL_AudioSpec spec{SDL_AUDIO_F32,2,48000};
        Uint8* wav=nullptr;Uint32 wavLength=0;std::vector<float> chime;
        const void* data=nullptr;int bytes=0;
        if(std::filesystem::exists(custom)&&SDL_LoadWAV(custom.c_str(),&spec,&wav,&wavLength)&&wavLength<64u*1024*1024){
            data=wav;bytes=int(wavLength);
        }else{
            if(wav){SDL_free(wav);wav=nullptr;}
            spec={SDL_AUDIO_F32,2,48000};chime=Chime(48000);data=chime.data();bytes=int(chime.size()*sizeof(float));
        }
        if(SDL_AudioStream* s=SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,&spec,nullptr,nullptr)){
            SDL_PutAudioStreamData(s,data,bytes);SDL_FlushAudioStream(s);SDL_ResumeAudioStreamDevice(s);
            for(int waited=0;SDL_GetAudioStreamQueued(s)>0&&waited<15000;waited+=40)SDL_Delay(40);
            SDL_Delay(200);SDL_DestroyAudioStream(s);
        }
        if(wav)SDL_free(wav);
    }).detach();
}
double NowSeconds(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
constexpr double PopIn=0.35,PopHold=5.2,PopOut=0.45;
}
bool TlgTrophiesToastActive(){std::lock_guard guard(popLock);return !pops.empty();}
void TlgTrophiesToast(float scale){
    int id;double t;bool first=false;
    {
        std::lock_guard guard(popLock);if(pops.empty())return;
        auto& p=pops.front();double now=NowSeconds();
        if(p.start<0){p.start=now;first=true;}
        t=now-p.start;id=p.id;
        if(t>PopIn+PopHold+PopOut){pops.pop_front();return;}
    }
    if(first)PlayTrophySound();
    bool fl;{std::lock_guard guard(lock);fl=firstLight;if(!ready)return;}
    const auto* text=IssTrophies::TextForTrophy(fl,id);
    // Slides in from the right edge and fades; out the same way.
    float a=t<PopIn?float(t/PopIn):(t>PopIn+PopHold?float(1.0-(t-PopIn-PopHold)/PopOut):1.0f);
    a=std::clamp(a,0.0f,1.0f);float ease=1.0f-(1.0f-a)*(1.0f-a);
    const ImGuiViewport* vp=ImGui::GetMainViewport();
    const float w=440.0f*scale,h=96.0f*scale,pad=24.0f*scale,icon=72.0f*scale,r=12.0f*scale;
    const float x1=vp->WorkPos.x+vp->WorkSize.x-pad+(1.0f-ease)*(w*0.35f),x0=x1-w,y0=vp->WorkPos.y+pad,y1=y0+h;
    auto* dl=ImGui::GetForegroundDrawList();
    auto col=[&](int R,int G,int B,float A){return IM_COL32(R,G,B,int(std::clamp(A*a,0.0f,1.0f)*255));};
    dl->AddRectFilled(ImVec2(x0,y0),ImVec2(x1,y1),col(14,14,18,0.92f),r);
    dl->AddRect(ImVec2(x0,y0),ImVec2(x1,y1),col(238,191,76,0.85f),r,0,1.5f*scale);
    const float ix=x0+12.0f*scale,iy=y0+(h-icon)/2;
    if(id>=0&&id<int(textures.size())&&textures[size_t(id)])
        dl->AddImageRounded(textures[size_t(id)]->GetTexRef(),ImVec2(ix,iy),ImVec2(ix+icon,iy+icon),ImVec2(0,0),ImVec2(1,1),col(255,255,255,1.0f),6.0f*scale);
    dl->AddRect(ImVec2(ix,iy),ImVec2(ix+icon,iy+icon),col(238,191,76,0.9f),6.0f*scale,0,1.5f*scale);
    const float tx=ix+icon+14.0f*scale;
    ImFont* font=ImGui::GetFont();
    dl->AddText(font,17.0f*scale,ImVec2(tx,y0+20.0f*scale),col(238,191,76,1.0f),id==0?"Platinum trophy earned":"You earned a trophy");
    std::string name=text?std::string(text->name):"Trophy "+std::to_string(id);
    dl->PushClipRect(ImVec2(tx,y0),ImVec2(x1-12.0f*scale,y1),true);
    dl->AddText(font,24.0f*scale,ImVec2(tx,y0+44.0f*scale),col(255,255,255,1.0f),name.c_str());
    dl->PopClipRect();
}
void TlgTrophiesMenu(){
    std::vector<int64_t> times;std::set<int> recovered;std::string error,identity;bool available,bound=false,allSaves=false;
    size_t thisSave=0;int n=48;bool fl=false;
#ifdef TLG_OVERLAY_PREVIEW
    fl=std::getenv("OVERLAY_PREVIEW_FIRST_LIGHT")!=nullptr;n=fl?25:48;available=true;allSaves=!fl;bound=true;
    times.assign(size_t(n),0);textures.assign(size_t(n),nullptr);thisSave=12;identity="preview";
    for(int id=1;id<n;id++)if(id%2==0)times[size_t(id)]=1700000000;
#else
    {
        std::lock_guard guard(lock);Init();available=ready;error=problem;allSaves=perSave&&globalOk;
        n=total;fl=firstLight;times.assign(size_t(n),0);
        const auto& shown=perSave?(allSaves?global:saveSession.Current()):progress;
        for(auto [id,time]:shown.earned)if(id>=0&&id<n)times[size_t(id)]=time;
        recovered=shown.recovered;
        if(perSave){bound=saveSession.Bound();identity=saveSession.Current().save_identity;thisSave=saveSession.Current().earned.size();}
    }
#endif
    OverlayDesign::Section(fl?"inFAMOUS First Light":"inFAMOUS Second Son");
    if(!available){ImGui::TextWrapped("Trophies unavailable: %s",error.c_str());return;}
    if(allSaves){
        OverlayDesign::Hint("Combined progress across your saves.");
    }
    else if(perSave){
        if(bound)OverlayDesign::Hint("Progress for the loaded save.");
        else ImGui::TextWrapped("Load a save or finish creating a new save to begin save-specific tracking.");
    }
    int earned=std::count_if(times.begin(),times.end(),[](int64_t t){return t>0;});
    char label[80];std::snprintf(label,sizeof(label),allSaves?"%d of %d trophies earned (all saves)":"%d of %d trophies earned",earned,n);
    ImGui::ProgressBar(earned/float(n),ImVec2(-1,ImGui::GetFrameHeight()),label);
    ImGui::Spacing();
    const float side=ImGui::GetFontSize()*3.4f;
    int columns=std::max(1,int(ImGui::GetContentRegionAvail().x/(side+ImGui::GetStyle().ItemSpacing.x*2)));
    if(ImGui::BeginTable("trophy_grid",columns,ImGuiTableFlags_SizingStretchSame)){
        for(int id=0;id<n;id++){
            ImGui::TableNextColumn();ImGui::PushID(id);
            ImVec2 pos=ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("trophy",ImVec2(side,side));
            bool hover=ImGui::IsItemHovered(),focus=ImGui::IsItemFocused();
            ImGui::GetWindowDrawList()->AddRectFilled(pos,ImVec2(pos.x+side,pos.y+side),times[id]?IM_COL32(42,64,86,255):IM_COL32(20,30,44,255),6);
            if(textures[id])ImGui::GetWindowDrawList()->AddImage(textures[id]->GetTexRef(),pos,ImVec2(pos.x+side,pos.y+side),ImVec2(0,0),ImVec2(1,1),IM_COL32(255,255,255,times[id]?255:90));
            if(times[id])ImGui::GetWindowDrawList()->AddRect(pos,ImVec2(pos.x+side,pos.y+side),IM_COL32(238,191,76,210),4);
            if(hover||focus){
                ImGui::BeginTooltip();
                const auto* text=IssTrophies::TextForTrophy(fl,id);
                if(text){
                    ImGui::TextUnformatted(text->name.data(),text->name.data()+text->name.size());
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+ImGui::GetFontSize()*28);
                    ImGui::TextUnformatted(text->requirement.data(),text->requirement.data()+text->requirement.size());
                    ImGui::PopTextWrapPos();ImGui::Separator();
                }
                ImGui::Text("Trophy %d: %s",id,times[id]?"earned":"not earned yet");
                if(recovered.contains(id))ImGui::TextUnformatted("Recovered from saved progression; original earning time unavailable.");
                else if(times[id]){
                    auto time=std::time_t(times[id]);std::tm tm{};
#ifdef _WIN32
                    localtime_s(&tm,&time);
#else
                    localtime_r(&time,&tm);
#endif
                    char stamp[64];std::strftime(stamp,sizeof(stamp),"Earned locally: %Y-%m-%d %H:%M",&tm);ImGui::TextUnformatted(stamp);
                }
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }ImGui::EndTable();
    }
    ImGui::Spacing();
    OverlayDesign::Hint("Hover or focus a trophy to see its name and requirement.");
    // Credit, in the launcher's credit green (Theme.ok #35d07f, ModsPage.qml).
    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0x35/255.0f,0xd0/255.0f,0x7f/255.0f,1.0f));
    ImGui::TextWrapped("Idea by (WOLF), Built by sunnyGeezy");
    ImGui::PopStyleColor();
    if (ImGui::CollapsingHeader("Tracking details")) {
    if(allSaves)ImGui::TextWrapped("New trophies are kept immediately across your saves. Platinum unlocks when the other %d trophies are earned.",n-1);
    if(perSave&&!allSaves)ImGui::TextWrapped("Save your game to retain new trophies. Recovery supports story decisions/missions (IDs 2-11) and saved event counters (17, 19, 22, 42). Other historical trophies are still under investigation. No PSN sync.");
    if(perSave)ImGui::TextWrapped("%s",collectibleRecovery?"Verified collectible recovery enabled (IDs 25-32).":"Collectible recovery disabled: game world/script dataset did not match. Live trophies and other tracking remain available.");
    else ImGui::TextWrapped("Local progress records trophies reported during play. Existing saves do not automatically import past trophies. No PSN sync.");
    if(fl)ImGui::TextWrapped("The platinum unlocks when the other 24 are earned.");
    if(bound)ImGui::Text("Loaded save: %zu of %d · %.12s",thisSave,n,identity.c_str());
    }
    if(!error.empty())ImGui::TextWrapped("Tracking / recovery status: %s",error.c_str());
}
#endif
