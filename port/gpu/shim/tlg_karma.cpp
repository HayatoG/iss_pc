#include "../../src/iss_karma.h"
#include "imgui.h"
#include "tlg_karma_backup.h"
#include "tlg_karma_profile_names.h"
#include "tlg_karma_recovery.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" const char *runtime_file_user_dir(void);
namespace {
std::atomic<bool> backing_up{false};
std::mutex message_lock;
std::string message;
std::vector<KarmaRecovery::Candidate> recovery_candidates;
void Message(std::string text){std::lock_guard guard(message_lock);message=std::move(text);}
void BackupProfile(const std::filesystem::path& root,const std::filesystem::path& destination){
    namespace fs=std::filesystem;
    TlgKarmaBackup(root/"savedata"/"1"/"CUSA00223",destination/"savedata");
    for(const auto* name:{"karma-original","karma-saves-v2","karma-profile-names","karma-profile-archive"}){
        if(!fs::exists(root/name))continue;
        bool has_file=false;for(const auto& entry:fs::recursive_directory_iterator(root/name))if(entry.is_regular_file()){has_file=true;break;}
        if(has_file)TlgKarmaBackup(root/name,destination/name);
    }
}
std::filesystem::path NewBackup(const std::filesystem::path& root,const char* action){
    const auto stamp=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return root/"karma-backups"/(std::to_string(stamp)+"-"+action);
}
void PrepareCapture(std::string name,bool replace){
    name=KarmaProfileNames::Normalize(name);
    if(backing_up.exchange(true))return;
    const std::filesystem::path root=runtime_file_user_dir();
    const std::string token=iss_karma_profile_token();
    Message("Backing up saves and reset tracking...");
    std::thread([root,token,name,replace]{try{
        if(!KarmaProfileNames::Valid(name))throw std::runtime_error("Enter a name of 1-80 bytes without surrounding spaces or control characters");
        BackupProfile(root,NewBackup(root,replace?"replace-reset-point":"capture-reset-point"));
        if(!iss_karma_profile_request_capture(token.c_str(),name.c_str(),replace))throw std::runtime_error("Save changed or capture is unavailable; save and reload first");
        Message("Backup verified. Close Insert to capture this reset point in gameplay.");
    }catch(const std::exception& e){Message(std::string("Capture cancelled: ")+e.what());}backing_up=false;}).detach();
}
void ScanRecovery(){
    if(backing_up.exchange(true))return;
    const std::filesystem::path root=runtime_file_user_dir();
    Message("Looking for earlier saves with verified matching tracking...");
    std::thread([root]{try{
        auto found=KarmaRecovery::Scan(root);std::lock_guard guard(message_lock);
        recovery_candidates=std::move(found);message="Select an earlier save to restore into a new manual slot. Existing slots are preserved.";
    }catch(const std::exception& e){Message(std::string("Recovery scan failed: ")+e.what());}backing_up=false;}).detach();
}
void RestoreRecovery(KarmaRecovery::Candidate candidate){
    if(backing_up.exchange(true))return;
    const std::filesystem::path root=runtime_file_user_dir();
    Message("Backing up current saves before recovery...");
    std::thread([root,candidate]{try{
        BackupProfile(root,NewBackup(root,"recover-earlier-save"));
        const auto slot=KarmaRecovery::RestoreNewSlot(root,candidate);
        Message("Earlier save copied to "+slot+". Reopen the game's Load Game menu and load that new manual slot; current gameplay has not changed.");
    }catch(const std::exception& e){Message(std::string("Recovery cancelled: ")+e.what());}backing_up=false;}).detach();
}
void Prepare(int mode){
    if(backing_up.exchange(true))return;
    Message("Backing up saves...");
    // Copy the root before starting the worker; no game memory is accessed there.
    const std::filesystem::path root=runtime_file_user_dir();
    const std::string token=iss_karma_profile_token();
    std::thread([root,mode,token]{
        namespace fs=std::filesystem;
        try{
            BackupProfile(root,NewBackup(root,mode==1?"true-hero":mode==2?"infamous":"reset"));
            if(token!=iss_karma_profile_token())throw std::runtime_error("Loaded save changed during backup");
            IssKarmaStatus status{};runtime_karma_status(&status);
            if(!status.supported || !status.ready)throw std::runtime_error("Return to gameplay and try again");
            if(mode!=3 && !status.enabled)throw std::runtime_error("Karma Switcher is disabled");
            runtime_karma_request(mode);
            Message("Backup verified. Switch queued for gameplay.");
        }catch(const std::exception& e){Message(std::string("Switch cancelled: ")+e.what());}
        backing_up.store(false);
    }).detach();
}
}
void TlgKarmaMenu(){
    IssKarmaStatus s{};runtime_karma_status(&s);
    ImGui::TextUnformatted("Karma Switcher");
    ImGui::Separator();
    ImGui::TextWrapped("Choose one karma branch at Level 5. Purchase upgrades normally in the Powers menu. The exact shard shortfall for both branches of earned powers is added. Switching takes effect when gameplay resumes.");
    if(!s.supported){ImGui::TextWrapped("Unavailable: %s",runtime_karma_compatibility_reason());return;}
    const bool bound=iss_karma_binding_ready()!=0;
    if(!bound)ImGui::TextWrapped("Karma changes blocked: %s",iss_karma_binding_problem());
    static char profile_name[81]="My reset point";static std::string last_token;
    const std::string token=iss_karma_profile_token();
    if(token!=last_token){last_token=token;const char* stored=iss_karma_profile_name();std::snprintf(profile_name,sizeof(profile_name),"%s",*stored?stored:"My reset point");}
    const char* current_name=iss_karma_profile_name();
    ImGui::Text("Reset profile: %s",*current_name?current_name:"Unnamed");
    ImGui::InputText("Reset profile name",profile_name,sizeof(profile_name));
    const bool pending_capture=iss_karma_profile_capture_pending()!=0;
    ImGui::BeginDisabled(backing_up.load()||pending_capture||!bound||!s.can_reset);
    if(ImGui::Button("Rename profile"))Message(iss_karma_profile_rename(token.c_str(),KarmaProfileNames::Normalize(profile_name).c_str())?"Profile renamed. Reset values were preserved.":"Could not rename this profile.");
    ImGui::EndDisabled();ImGui::SameLine();
    static bool replacing=false,acknowledged=false;
    ImGui::BeginDisabled(backing_up.load()||pending_capture||!iss_karma_profile_can_capture()||s.mode!=0);
    if(ImGui::Button("Capture current progression")){replacing=false;acknowledged=false;ImGui::OpenPopup("Capture reset point");}
    ImGui::EndDisabled();
    ImGui::BeginDisabled(backing_up.load()||pending_capture||!iss_karma_profile_can_replace()||s.mode!=0);
    if(ImGui::Button("Archive and replace reset point")){replacing=true;acknowledged=false;ImGui::OpenPopup("Capture reset point");}
    ImGui::EndDisabled();
    ImGui::TextWrapped("For a new reset point, save your current progress and reload it first. Stop the selected Karma mode before capturing. Names are labels; they never choose which save or values Reset uses.");
    if(ImGui::BeginPopupModal("Capture reset point",nullptr,ImGuiWindowFlags_AlwaysAutoResize)){
        ImGui::Text("Profile: %s",profile_name);
        ImGui::Text("Current: %s Level %d, %.1f karma, %d shards",s.side==1?"Good":"Evil",s.rank,s.score,s.shards);
        ImGui::TextWrapped("This captures the currently loaded karma, shards and upgrades. Already boosted values or modded upgrades will become part of this baseline. Earlier natural progression cannot be inferred.");
        if(replacing)ImGui::TextWrapped("The previous reset record and name will be archived before replacement. This change applies to this exact saved point; older and other saves keep their own records.");
        ImGui::Checkbox("I want these current values as my reset point",&acknowledged);
        const auto clean_name=KarmaProfileNames::Normalize(profile_name);
        if(!KarmaProfileNames::Valid(clean_name))ImGui::TextWrapped("Enter a name of 1-80 bytes without control characters. Surrounding spaces are trimmed.");
        ImGui::BeginDisabled(!acknowledged||!KarmaProfileNames::Valid(clean_name)||backing_up.load());
        if(ImGui::Button("Back up and capture")){PrepareCapture(profile_name,replacing);ImGui::CloseCurrentPopup();}
        ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("Cancel"))ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if(iss_karma_profile_capture_result()==1)ImGui::TextWrapped("Reset point captured. Current karma, shards and upgrades were preserved.");
    if(iss_karma_profile_capture_result()<0)ImGui::TextWrapped("Capture could not complete: %s",iss_karma_binding_problem());
    if(ImGui::CollapsingHeader("Recover an earlier baseline")){
        ImGui::TextWrapped("Only earlier saves with verified matching tracking are offered. Recovery copies one into a new manual slot. Load that slot through the game's Load Game menu to use its own baseline.");
        ImGui::BeginDisabled(backing_up.load()||pending_capture);
        if(ImGui::Button("Find verified earlier saves"))ScanRecovery();
        std::vector<KarmaRecovery::Candidate> candidates;{std::lock_guard guard(message_lock);candidates=recovery_candidates;}
        static int selected_recovery=-1;
        for(size_t i=0;i<candidates.size();++i){ImGui::PushID(int(i));
            if(ImGui::Selectable(candidates[i].name.c_str(),selected_recovery==int(i)))selected_recovery=int(i);
            if(selected_recovery==int(i))ImGui::Text("Baseline %.1f | Level %d | Saved shards %d",candidates[i].baseline,candidates[i].rank,candidates[i].shards);
            ImGui::PopID();}
        ImGui::BeginDisabled(selected_recovery<0||selected_recovery>=int(candidates.size()));
        if(ImGui::Button("Restore selected as new manual slot"))RestoreRecovery(candidates[selected_recovery]);
        ImGui::EndDisabled();ImGui::EndDisabled();
    }
    ImGui::Separator();
    bool enabled=s.enabled!=0;
    if(ImGui::Checkbox("Enable Karma Switcher",&enabled))runtime_karma_set_enabled(enabled);
    ImGui::TextWrapped("Unchecking stops the maximum-karma hold and disables switching. Reset remains available. Re-enabling does not select a mode automatically. This checkbox applies to this session.");
    ImGui::Text("Current: %s | Level %d | %.1f",s.side==1?"Good":"Evil",s.rank,s.score);
    ImGui::Text("Unspent shards: %d | Remaining upgrade cost (both branches): %d",s.shards,s.remaining_cost);
    ImGui::Text("Unused bonus shards: %d",s.bonus_shards);
    if(s.shard_error)ImGui::TextWrapped("Shard tracking mismatch or storage failure. Currency changes blocked; Reset will keep your shard balance unchanged.");
    ImGui::Text("Assisted purchases to undo: %d",s.rollback_count);
    if(s.rollback_error)ImGui::TextWrapped("Purchase journal error. Progression rollback is blocked; your verified backups remain available.");
    ImGui::Text("Selected mode: %s",s.mode==1?"True Hero":s.mode==2?"Infamous":"Unchanged");
    ImGui::BeginDisabled(!bound || !s.ready || s.pending || backing_up.load()||pending_capture);
    ImGui::BeginDisabled(!enabled);
    if(ImGui::Button("True Hero"))Prepare(1);
    ImGui::SameLine();
    if(ImGui::Button("Infamous"))Prepare(2);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!s.can_reset);
    ImGui::SameLine();
    if(ImGui::Button("Reset karma"))Prepare(3);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if(s.can_reset)ImGui::Text("Reset target: %s | Level %d | %.1f",s.original_side==1?"Good":"Evil",s.original_rank,s.original_score);
    ImGui::TextWrapped("Original karma is saved to disk before your first switch and survives restarts and altered saves. Reset undoes recorded purchases made with bonus shards or pre-boost karma restrictions, refunds their natural-shard portion, and removes unused bonus shards. Missions, collectibles and story power families are not rewound.");
    ImGui::TextWrapped("Tracking is bound to the loaded save contents. Loading another save stops the selected mode and loads that save's own tracking. Legacy profile-wide baselines are never adopted automatically.");
    if(s.error==4)ImGui::TextWrapped("Original snapshot could not be read or safely stored. Switching is blocked to protect it.");
    if(s.error==8)ImGui::TextWrapped("Progress changed before the first boost. Save to a new slot and reload it before creating this save's baseline.");
    if(!s.ready)ImGui::TextDisabled("Load a save and enter gameplay first.");
    if(s.error)ImGui::TextWrapped("Switch could not complete (%d). Your backup remains available.",s.error);
    {std::lock_guard guard(message_lock);ImGui::TextWrapped("%s",message.c_str());}
    ImGui::Spacing();
    ImGui::TextWrapped("A verified backup is made before each switch, in karma-backups inside your data folder. Story progress and collectibles are not edited. Normal autosaves may record karma, shard balance and purchased upgrades.");
    ImGui::TextDisabled("The selected mode keeps karma at maximum for this session.");
}
