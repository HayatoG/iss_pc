// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "imgui.h"
#include "overlay_design.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <set>
#include <chrono>
#include <stdexcept>
#include <cstring>
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#endif
extern "C" int runtime_overlay_mod_available(int);
extern "C" int runtime_overlay_mod_get(int);
extern "C" int runtime_overlay_mod_set(int,int);
extern "C" const char* runtime_file_user_dir();
extern "C" void runtime_restart();

namespace OverlayMods {
using Json=nlohmann::json;
namespace fs=std::filesystem;
struct Mod {std::string key,name,note;bool active=false,wanted=false;};
inline int LiveId(const std::string&key){
    if(key=="iss_godmode.xml/God mode")return 2;
    if(key=="iss_infinite_energy.xml/Unlimited energy")return 3;
    if(key=="iss_infinite_energy.xml/Unlimited missiles")return 4;
    if(key=="iss_video_dash_endless.xml/Endless video dash")return 5;
    return key=="native/camera-speed-1.5x"?0:key=="iss_builtin_intro.xml/Disable motion blur"?1:-1;
}
inline const char* LiveHint(int id){
    switch(id){
    case 0:return "Changes camera speed and the pitch slowdown safeguard immediately.";
    case 1:return "Changes the engine motion-blur flag immediately. Disabling restores its original value.";
    case 2:return "Prevents damage from lowering health while enabled. Does not refill health already lost.";
    case 3:return "Prevents energy drain, including charged shots, while enabled. Normal drain resumes when disabled.";
    case 4:return "The next missile-count update keeps five missiles while enabled. Normal consumption resumes when disabled.";
    case 5:return "Video dash continues while held. Normal timing and speed return when disabled.";
    default:return "";
    }
}
inline Json Read(const fs::path&path,bool missing=false){
    if(fs::is_symlink(path))throw std::runtime_error("Linked mod files are not supported.");
    if(!fs::exists(path)){if(missing)return Json::object();throw std::runtime_error("Start this game through the updated launcher to load its mods.");}
    if(fs::file_size(path)>2*1024*1024)throw std::runtime_error("Mod configuration is too large.");
    std::ifstream file(path);auto data=Json::parse(file);if(!data.is_object())throw std::runtime_error("Invalid mod configuration.");return data;
}
inline bool Desired(const Json&config,const std::string&key,bool fallback){
    bool on=false,off=false;
    for(const char*field:{"enabled","disabled"})if(config.contains(field)){
        if(!config[field].is_array())throw std::runtime_error("Invalid mod selection list.");
        for(const auto&entry:config[field]){if(!entry.is_string())throw std::runtime_error("Invalid mod selection key.");if(entry==key)(field[0]=='e'?on:off)=true;}
    }
    return off?false:on?true:fallback;
}
inline void WriteSelection(const fs::path&path,const std::string&key,bool enabled){
    auto config=Read(path,true);std::set<std::string> on,off;
    (void)Desired(config,key,false); // Validate before any write; never reset malformed files.
    for(const auto&entry:config.value("enabled",Json::array()))on.insert(entry.get<std::string>());
    for(const auto&entry:config.value("disabled",Json::array()))off.insert(entry.get<std::string>());
    if(key=="native/camera-speed-1.5x"){on.erase("native/camera-prevent-pitch-slowdown");off.erase("native/camera-prevent-pitch-slowdown");}
    (enabled?on:off).insert(key);(enabled?off:on).erase(key);
    config["enabled"]=on;config["disabled"]=off;
    const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path temp=path;temp += ".overlay-"+std::to_string(stamp)+".tmp";
    try{
        {std::ofstream out(temp,std::ios::binary|std::ios::trunc);out<<config.dump(2);out.flush();if(!out)throw std::runtime_error("Could not save mod selection.");}
#ifdef _WIN32
        if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Mod selection could not be replaced; it was not changed.");
#else
        fs::rename(temp,path);
#endif
    }catch(...){std::error_code ec;fs::remove(temp,ec);throw;}
}
inline void Draw(const char*game,bool portuguese){
    static std::vector<Mod> mods;static fs::path config;static std::string loaded,error,message;static auto checked=std::chrono::steady_clock::time_point{};
#ifdef TLG_OVERLAY_PREVIEW
    const fs::path root=std::getenv("OVERLAY_PREVIEW_MODS_ROOT")?std::getenv("OVERLAY_PREVIEW_MODS_ROOT"):".";
#else
    const fs::path root=runtime_file_user_dir();
#endif
    auto load=[&]{
        mods.clear();error.clear();
        try{
            auto catalogue=Read(root/"overlay-mods.json");
            if(catalogue.value("schema",0)!=1||catalogue.value("game",std::string{})!=game||!catalogue.contains("mods")||!catalogue["mods"].is_array())throw std::runtime_error("The mods catalogue does not match this game.");
            config=root/"mods.json";
            if(const char*custom=std::getenv("ISS_MODS_CONFIG");custom&&*custom)config=custom;
            if(fs::weakly_canonical(config)!=fs::weakly_canonical(fs::path(catalogue.at("config").get<std::string>())))throw std::runtime_error("The mod configuration path changed. Restart through the launcher.");
            auto choices=Read(config,true);std::set<std::string> keys;
            for(const auto&row:catalogue["mods"]){
                Mod mod{row.at("key").get<std::string>(),row.at("name").get<std::string>(),row.value("note",std::string{}),row.value("active",false)};
                if(mod.key.empty()||mod.key.size()>1024||!keys.insert(mod.key).second)throw std::runtime_error("Invalid mod catalogue key.");
                mod.wanted=Desired(choices,mod.key,mod.active);mods.push_back(std::move(mod));
            }
        }catch(const std::exception&e){mods.clear();error=e.what();}
        checked=std::chrono::steady_clock::now();loaded=root.string()+"/"+game;
    };
    if(loaded!=root.string()+"/"+game||std::chrono::steady_clock::now()-checked>std::chrono::seconds(2))load();
    OverlayDesign::Section(!std::strcmp(game,"first_light")?"inFAMOUS First Light":"inFAMOUS Second Son");
    OverlayDesign::Hint(portuguese?"Os mods marcados como Ao vivo mudam imediatamente. Mods de inicialização valem na próxima partida.":"Live mods apply immediately. Startup-only mods take effect on the next launch.");
    if(ImGui::Button(portuguese?"Atualizar estados":"Refresh states"))load();
    if(!error.empty()){ImGui::TextWrapped("%s",error.c_str());return;}
    if(mods.empty()){OverlayDesign::Hint(portuguese?"Nenhum mod compatível instalado para este jogo.":"No compatible mods are installed for this game.");return;}
    int pending=0;
    for(auto&mod:mods){
        const int id=LiveId(mod.key);
#ifdef TLG_OVERLAY_PREVIEW
        const bool live=id>=0;
        const bool active=mod.active;
#else
        const bool live=id>=0&&runtime_overlay_mod_available(id);
        const bool active=live?runtime_overlay_mod_get(id)!=0:mod.active;
#endif
        ImGui::PushID(mod.key.c_str());ImGui::Spacing();
        const float control=ImGui::GetFrameHeight()+12;
        if(ImGui::BeginTable("mod_row",2,ImGuiTableFlags_SizingStretchProp|ImGuiTableFlags_RowBg)){
            ImGui::TableSetupColumn("mod",ImGuiTableColumnFlags_WidthStretch);ImGui::TableSetupColumn("switch",ImGuiTableColumnFlags_WidthFixed,control);
            ImGui::TableNextColumn();ImGui::TextWrapped("%s",mod.name.c_str());
            ImGui::TextColored(live?ImVec4(.4f,.9f,.7f,1):ImVec4(.72f,.76f,.84f,1),"%s",live?(portuguese?"Ao vivo":"Live"):(id>=0?(portuguese?"Ao vivo indisponível nesta versão":"Live control unavailable on this build"):(portuguese?"Inicialização":"Startup only")));
            if(ImGui::IsItemHovered()&&(live||!mod.note.empty()))ImGui::SetTooltip("%s",live?LiveHint(id):mod.note.c_str());
            ImGui::TableNextColumn();bool wanted=mod.wanted;
            if(ImGui::Checkbox("##enabled",&wanted)){
                try{WriteSelection(config,mod.key,wanted);mod.wanted=wanted;
#ifndef TLG_OVERLAY_PREVIEW
                    if(live&&!runtime_overlay_mod_set(id,wanted))throw std::runtime_error("Live change could not be applied; selection saved for next launch.");
#endif
                    message=live?(portuguese?"Mod aplicado ao vivo.":"Mod applied live."):(portuguese?"Seleção salva para a próxima partida.":"Selection saved for the next launch.");
                }catch(const std::exception&e){message=e.what();}
            }
            ImGui::EndTable();
        }
        if(mod.wanted!=active){++pending;OverlayDesign::Hint(live?(portuguese?"Selecionado difere do estado ativo.":"Selected state differs from active state."):(portuguese?"Alteração pendente para a próxima partida.":"Change pending for the next launch."));}
        ImGui::PopID();
    }
    if(!message.empty()){ImGui::Spacing();ImGui::TextWrapped("%s",message.c_str());}
    if(pending){
        ImGui::Spacing();OverlayDesign::Hint(portuguese?"Mods pendentes: salve seu progresso, feche o jogo e inicie pelo launcher.":"Pending mods: save your progress, exit the game, then start it through the launcher.");
    }
}
}
