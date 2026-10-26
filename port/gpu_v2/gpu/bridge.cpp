// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef _WIN32
// First: system headers below (SDL3, libc++) may bring <windows.h>, hence the old WinSock.h
// boost.asio refuses, before the include further down.
#include <winsock2.h>
#endif
#include "iss_gpu.h"
#include "native_shadow.h"
#include "native_gbuffer.h"
#include "native_passes.h"
#include "pause_guard.h"
#include "guest_heap_census.h"
#include "repeat_census.h"
namespace iss::native_gbuffer { int InstallCensus(std::uint64_t base); }
#include "sparse_copy.h"
#include "readback_hint.h"
#include "deep_profile.h"
#include "compute_phase.h"
#include "infamous_renderer/feedback_identity_registry.h"
#include "runtime_timeline.h"
#include <SDL3/SDL.h>
#include <atomic>
// The shader preload's progress (vk_pipeline_serialization.cpp): entries read, phase
// (0 not started, 1 reading, 2 finishing the compiles, 3 done), shown in the window title.
namespace iss { extern std::atomic<unsigned> g_preload_entries, g_preload_total; extern std::atomic<int> g_preload_phase; }
#include <atomic>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include "g4_conditional_backing_write.h"
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
// boost.asio needs Winsock 2 before <windows.h> pulls in the old WinSock.h.
#ifdef _WIN32
#include <winsock2.h>
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <xxhash.h>
#include "common/elf_info.h"
#include "common/memory_patcher.h"
#include "common/thread.h"
#include "common/path_util.h"
#include "common/perf_stats.h"
#include "common/rdtsc.h"
#include "common/signal_context.h"
#include "core/libraries/libs.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/kernel/process.h"
#include "core/memory.h"
#include "core/signals.h"
#include "sdl_window.h"
#include "video_core/renderdoc.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "tlg_overlay.h"
#include <imgui.h>
#include "video_core/renderer_vulkan/vk_rasterizer.h"
namespace iss::diagnostics {void RecordReadbackTime(u64 rip,u64 address,u64 elapsed,unsigned kind);}
namespace Vulkan{class Presenter;}
extern std::unique_ptr<Vulkan::Presenter> presenter; // gnmdriver.cpp
// The process ends (window closed, ISS_STOP_FILE): the host's exit report first (gpu_host.cpp).
[[noreturn]] void IssHostExit();
// A benchmark window ended with ISS_BENCH_EXIT=1 (gpu_host.cpp, any thread): stops as ISS_STOP_FILE.
bool IssHostStopRequested();
extern "C" void iss_gpu_install_image(uint64_t image_base);

Frontend::WindowSDL *g_window=nullptr;
namespace Libraries::GnmDriver {void RegisterLib(Core::Loader::SymbolsResolver*);}
namespace Libraries::VideoOut {void RegisterLib(Core::Loader::SymbolsResolver*);}
namespace Libraries::Kernel {void RegisterEventQueue(Core::Loader::SymbolsResolver*);}
namespace {
IssGpuCallbacks host{};
struct HostAliasMapping {u64 guest{}, size{}, physical{};};
thread_local std::vector<HostAliasMapping>* alias_visit_output{};
ReadbackHints readback_hints;
bool precise_reduction_site{};
bool precise_feedback_site{};
std::atomic<bool> explicit_readback_hints{};
bool FeedbackIdentityEnabled(){
    static const bool enabled=[]{const char* flag=std::getenv("ISS_FEEDBACK_IDENTITY");
        return flag&&flag[0]=='1';}();
    return enabled;
}
struct Symbol {std::string nid,library,module;uintptr_t pointer;};
std::vector<Symbol> symbols;
std::mutex mappings_mutex;
boost::icl::interval_set<VAddr> mappings;
std::thread window_thread;
std::mutex ready_mutex;
std::condition_variable ready_cv;
int ready=0;
/// ISS_THREAD_PRIORITY=1: the process runs above normal priority (as the emulator did) and the
/// threads that translate and present the graphics run one level above the game threads, so that
/// other programs and the game's own threads delay them less. Pipeline compilers stay low.
bool thread_priority_enabled(){static const bool on=[]{const char *p=std::getenv("ISS_THREAD_PRIORITY");return p&&p[0]=='1';}();return on;}
#ifndef _WIN32
// Linux: thread priorities are left to the scheduler (raising them needs CAP_SYS_NICE).
void raise_gpu_threads(){}
#else
void raise_gpu_threads(){
    // ISS_PENDING_OPS_PRIO=1 (default 0): also the thread that writes the deferred labels, EOPs
    // and interrupts the guest waits on once the GPU reaches their tick (vk_scheduler.cpp).
    static const bool pending_ops=[]{const char *p=std::getenv("ISS_PENDING_OPS_PRIO");return p&&p[0]=='1';}();
    static const wchar_t *names[]={L"shadPS4:GpuCommandProcessor",L"shadPS4:GpuCommandRecorder",L"shadPS4:PresentThread",L"shadPS4:GpuUploadWorker",L"iss:DrawStageB",
                                   pending_ops?L"shadPS4:GpuSchedPriorityPendingOpsRunner":nullptr};
    const DWORD pid=GetCurrentProcessId();
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);if(snap==INVALID_HANDLE_VALUE)return;
    THREADENTRY32 te{};te.dwSize=sizeof(te);
    for(BOOL ok=Thread32First(snap,&te);ok;ok=Thread32Next(snap,&te)){
        if(te.th32OwnerProcessID!=pid)continue;
        HANDLE t=OpenThread(THREAD_SET_LIMITED_INFORMATION|THREAD_QUERY_LIMITED_INFORMATION,FALSE,te.th32ThreadID);if(!t)continue;
        PWSTR desc=nullptr;
        if(SUCCEEDED(GetThreadDescription(t,&desc))&&desc){
            for(const wchar_t *n:names)if(n&&!wcscmp(desc,n)&&GetThreadPriority(t)<THREAD_PRIORITY_ABOVE_NORMAL){
                if(SetThreadPriority(t,THREAD_PRIORITY_ABOVE_NORMAL))std::fprintf(stderr,"PORT_TUNING thread_priority %ls=above_normal\n",desc);
            }
            LocalFree(desc);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
}
// ISS_HIGH_QOS (default 1; 0 skips it): Windows 11 may run a process or thread it judges to be
// in the background with EcoQoS (on E-cores, at low clocks). This opts the process and the command
// processor out of execution-speed throttling. Best effort: the calls are looked up at run time.
bool high_qos_enabled() {
    static const bool on = [] { const char* p = std::getenv("ISS_HIGH_QOS"); return !p || p[0] != '0'; }();
    return on;
}
struct IssPowerThrottlingState { ULONG Version, ControlMask, StateMask; };
constexpr ULONG kPowerThrottlingVersion = 1, kPowerThrottlingExecutionSpeed = 0x1;
constexpr int kProcessPowerThrottling = 4, kThreadPowerThrottling = 3;
using SetInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
// thread=false: the process; thread=true: the calling thread.
void iss_set_high_qos(bool thread) {
    if (!high_qos_enabled()) {
        if (!thread) std::fprintf(stderr, "PORT_TUNING high_qos=0 (ISS_HIGH_QOS=0)\n");
        return;
    }
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    const auto set = kernel ? reinterpret_cast<SetInformationFn>(reinterpret_cast<void*>(GetProcAddress(
                                  kernel, thread ? "SetThreadInformation" : "SetProcessInformation")))
                            : nullptr;
    if (!set) {
        std::fprintf(stderr, "PORT_TUNING high_qos %s unavailable\n", thread ? "thread" : "process");
        return;
    }
    IssPowerThrottlingState state{kPowerThrottlingVersion, kPowerThrottlingExecutionSpeed, 0};
    const BOOL ok = thread ? set(GetCurrentThread(), kThreadPowerThrottling, &state, sizeof(state))
                           : set(GetCurrentProcess(), kProcessPowerThrottling, &state, sizeof(state));
    std::fprintf(stderr, "PORT_TUNING high_qos=1 %s ok=%d%s\n", thread ? "thread=command_processor" : "process",
                 ok ? 1 : 0, ok ? "" : " (EcoQoS opt-out refused)");
}
#endif
}
namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char *nid,const char *library,const char *module,SymbolType,u64 address){symbols.push_back({nid,library,module,address});}
}
namespace Core {
class Emulator {
public:static void Configure(){auto& e=Common::ElfInfo::Instance();e.initialized=true;e.game_serial=host.serial;e.title=host.title;e.sdk_ver=host.sdk_version;e.psf_attributes.raw=host.psf_attributes;e.app_ver="01.07";}
};
void AddressSpace::Register(VAddr address,u64 size,u8 *backing){
    if(!size)return;
    std::scoped_lock guard{mappings_mutex};mappings.add(boost::icl::interval<VAddr>::right_open(address,address+size));
    for(u64 at=address;at<address+size;at+=16384)backing_pages[at>>14]=backing+(at-address);
}
boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions(){std::scoped_lock guard{mappings_mutex};return mappings;}
void AddressSpace::Protect(VAddr address,u64 size,MemoryPermission perms){
    if(!host.protect(address,size,static_cast<u32>(perms)))throw std::runtime_error("GPU protection failed");
}
void MemoryManager::SetRasterizer(Vulkan::Rasterizer *value){rasterizer=value;
    host.visit_mappings([](uint64_t address,uint64_t size,uint8_t *backing){iss_gpu_map(address,size,backing);});
}
u64 MemoryManager::ClampRangeSize(VAddr address,u64 size){return host.clamp(address,size);}
std::optional<MemoryManager::BackingSpan> MemoryManager::DescribeBacking(VAddr address,u64 size){
    if(size==0)return std::nullopt;
    const u64 contiguous=host.clamp(address,size);
    if(contiguous==0)return std::nullopt;
    u8* backing=host.backing(address,contiguous);
    if(!backing)return std::nullopt;
    return BackingSpan{reinterpret_cast<u64>(backing),contiguous};
}
std::optional<std::vector<MemoryManager::BackingAlias>>
MemoryManager::EnumerateBackingAliases(VAddr address,u64 size){
    const auto requested=DescribeBacking(address,size);
    if(!requested||requested->size!=size||
       requested->physical_start>std::numeric_limits<u64>::max()-size)return std::nullopt;
    std::vector<HostAliasMapping> mappings_snapshot;
    mappings_snapshot.reserve(4096); // native mapping table is bounded to 4096 entries
    auto* previous=alias_visit_output;
    alias_visit_output=&mappings_snapshot;
    host.visit_mappings([](u64 guest,u64 length,u8* physical){
        if(alias_visit_output)alias_visit_output->push_back(
            HostAliasMapping{guest,length,reinterpret_cast<u64>(physical)});
    });
    alias_visit_output=previous;
    const u64 physical_end=requested->physical_start+size;
    std::vector<BackingAlias> aliases;
    for(const auto& mapping:mappings_snapshot){
        if(mapping.size==0||mapping.guest>std::numeric_limits<u64>::max()-mapping.size||
           mapping.physical>std::numeric_limits<u64>::max()-mapping.size)return std::nullopt;
        const u64 lo=std::max(requested->physical_start,mapping.physical);
        const u64 hi=std::min(physical_end,mapping.physical+mapping.size);
        if(lo>=hi)continue;
        const u64 guest= mapping.guest+(lo-mapping.physical);
        // A CPU-only VA alias can still write the same physical bytes. The SRT provider
        // must arm it too, or reject the snapshot if PageManager cannot protect it.
        // The native table is append-only. A replaced VA or stale earlier entry must not
        // silently authorize a snapshot of the wrong physical bytes.
        if(host.backing(guest,hi-lo)!=reinterpret_cast<u8*>(lo))return std::nullopt;
        for(const auto& other:mappings_snapshot){
            if(&other==&mapping||other.size==0)continue;
            if(other.guest>std::numeric_limits<u64>::max()-other.size||
               other.physical>std::numeric_limits<u64>::max()-other.size)
                return std::nullopt;
            const u64 overlap_lo=std::max(guest,other.guest);
            const u64 overlap_hi=std::min(guest+(hi-lo),other.guest+other.size);
            if(overlap_lo<overlap_hi &&
               lo+(overlap_lo-guest)!=other.physical+(overlap_lo-other.guest))
                return std::nullopt;
        }
        aliases.push_back(BackingAlias{guest,lo,hi-lo});
        if(aliases.size()>64)return std::nullopt;
    }
    if(aliases.empty())return std::nullopt;
    std::sort(aliases.begin(),aliases.end(),[](const auto& a,const auto& b){
        return a.guest_address<b.guest_address;
    });
    aliases.erase(std::unique(aliases.begin(),aliases.end(),[](const auto& a,const auto& b){
        return a.guest_address==b.guest_address && a.size==b.size &&
               a.physical_start==b.physical_start;
    }),aliases.end());
    return aliases;
}
u64 MemoryManager::MappingEpoch() const noexcept {
    return host.mapping_epoch ? host.mapping_epoch() : 0;
}
bool MemoryManager::WithMappingReadLock(const std::function<bool(u64)>& publish) const {
    if (!host.mapping_lock_shared || !host.mapping_unlock_shared) return false;
    const u64 epoch=host.mapping_lock_shared();
    try {
        const bool accepted=publish(epoch);
        host.mapping_unlock_shared();
        return accepted;
    } catch (...) {
        host.mapping_unlock_shared();
        throw;
    }
}
u64 MemoryManager::PreferredReadbackWindow(VAddr address,u64 fallback){return precise_reduction_site||precise_feedback_site||explicit_readback_hints.load(std::memory_order_relaxed)?readback_hints.WindowFor(address,fallback):fallback;}
void MemoryManager::CopySparseMemory(VAddr address,u8 *dest,u64 size){
    static const bool bulk=[] {const char *p=std::getenv("ISS_BULK_COPY");return !p||p[0]!='0';}();
    if(!bulk){while(size){const u64 part=std::min<u64>(size,16384-(address&16383));
        if(auto *source=host.backing(address,part))std::memcpy(dest,source,part);else std::memset(dest,0,part);
        address+=part;dest+=part;size-=part;}return;}
    iss_copy_sparse(address,dest,size,host.backing);
}
u64 MemoryManager::HashSparseMemory(VAddr address,u64 size){std::vector<u8> bytes(size);CopySparseMemory(address,bytes.data(),size);return XXH3_64bits(bytes.data(),bytes.size());}
bool MemoryManager::TryWriteBacking(void *address,const void *data,u64 size){
    const auto guest=reinterpret_cast<uintptr_t>(address);
    auto *dest=host.backing(guest,size);
    if(!dest)return false;
    if(rasterizer)rasterizer->WithSrtDirectWrite([&]{std::memcpy(dest,data,size);});
    else std::memcpy(dest,data,size);
    return true;
}
} // namespace Core
// native_passes.cpp segment tokens: a non-faulting write of a few bytes through the host backing
// (TryWriteBacking would invalidate every active SRT write watch under the global direct write
// gate, once per segment start). Written first, then only the watches of the pages written are
// invalidated: a watch armed before the invalidation is dropped by it, one armed after it reads
// the new bytes. False without a backing (nothing written).
bool IssWriteBackingRange(void* address,const void* data,u64 size){
    const auto guest=reinterpret_cast<uintptr_t>(address);
    auto* dest=host.backing(guest,size);
    if(!dest)return false;
    std::memcpy(dest,data,size);
    if(auto* rasterizer=Core::Memory::Instance()->GetRasterizer())
        rasterizer->InvalidateSrtWriteWatches(static_cast<VAddr>(guest),size);
    return true;
}
namespace Core {
bool MemoryManager::TryWriteBackingIf(void* address,const void* data,u64 size,
                                      u64 expected_native_epoch,
                                      const std::function<bool()>& predicate){
    if(!data||!size||!expected_native_epoch||!predicate||
       !host.mapping_lock_shared||!host.mapping_unlock_shared)return false;
    const auto guest=reinterpret_cast<uintptr_t>(address);
    // Resolve before taking the outer lock: host.backing takes its own shared
    // SRW lock and SRW locks do not promise recursive shared acquisition.
    auto* dest=host.backing(guest,size);
    if(!dest)return false;
    return iss::g4::ConditionalBackingWrite(
        [&](const auto& action){return WithMappingReadLock(action);},
        expected_native_epoch,predicate,[&]{
        if(rasterizer)rasterizer->WithSrtDirectWrite([&]{std::memcpy(dest,data,size);});
        else std::memcpy(dest,data,size);
    });
}
void MemoryManager::InvalidateMemory(VAddr address,u64 size){if(rasterizer)rasterizer->InvalidateMemory(address,size);}
}
namespace Libraries::Kernel {
boost::asio::io_context io_context;
static std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work;
static std::thread service_thread;
void KernelSignalRequest(){} // Work guard keeps the timer executor alive.
int *PS4_SYSV_ABI __Error(){return host.errno_location();}
u64 PS4_SYSV_ABI sceKernelGetTscFrequency(){return host.tsc_frequency();}
u64 PS4_SYSV_ABI sceKernelReadTsc(){return Common::FencedRDTSC();}
u64 PS4_SYSV_ABI sceKernelGetProcessTime(){return host.process_time_us();}
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter(){return host.process_time_us();}
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency(){return 1000000;}
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32 *out){if(!out)return static_cast<s32>(0x80020016);*out=host.sdk_version;return 0;}
s32 PS4_SYSV_ABI sceKernelIsNeoMode(){return 0;}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 us){std::this_thread::sleep_for(std::chrono::microseconds(us));return 0;}
}
namespace Libraries::SystemService {bool IsSplashVisible(){return false;}}
namespace Core::PerfSampler {
void PollTuning(){iss::diagnostics::Poll();}
void RecordReadback(unsigned long long address,unsigned long long rip){
    static const bool trace=[] {const char *p=std::getenv("ISS_TRACE_READBACK");return p&&p[0]=='1';}();if(!trace)return;
    static std::mutex mutex;static std::map<unsigned long long,unsigned long long> counts;
    std::scoped_lock guard{mutex};if(counts.size()>=64&&!counts.contains(rip))return;
    auto count=++counts[rip];if(count<=4||(count&(count-1))==0)std::fprintf(stderr,"READBACK_SITE rip=0x%llx address=0x%llx count=%llu\n",rip,address,count);
}}
namespace VideoCore {
static std::atomic<u32> screenshot_requests,overlay_screenshot_requests;
void LoadRenderDoc(){}void StartCapture(){}void EndCapture(){}void TriggerCapture(){}
void SetOutputDir(const std::filesystem::path&,const std::string&){}
bool IsRenderDocLoaded(){return false;}
void RequestScreenshot(ScreenshotRequest request){if(request==ScreenshotRequest::GameOnly)++screenshot_requests;else if(request==ScreenshotRequest::WithOverlays)++overlay_screenshot_requests;}
u32 ConsumeGameOnlyScreenshotRequests(){return screenshot_requests.exchange(0);}
u32 ConsumeWithOverlaysScreenshotRequests(){return overlay_screenshot_requests.exchange(0);}
ScreenshotRequests ConsumeScreenshotRequests(){return {ConsumeGameOnlyScreenshotRequests(),ConsumeWithOverlaysScreenshotRequests()};}
}
namespace Common::FS {
static std::map<PathType,std::filesystem::path> paths;
const std::filesystem::path& GetUserPath(PathType type){return paths.at(type);}
std::string GetUserPathString(PathType type){return GetUserPath(type).string();}
void SetUserPath(PathType type,const std::filesystem::path& path){std::filesystem::create_directories(path);paths[type]=path;}
bool ValidatePath(const std::filesystem::path& path){return !path.empty();}
std::string PathToUTF8String(const std::filesystem::path& path){auto s=path.u8string();return std::string(s.begin(),s.end());}
}
extern "C" void iss_gpu_map(uint64_t address,uint64_t size,uint8_t *backing){
    if(FeedbackIdentityEnabled())
        iss::game::feedback::IdentityRegistry::Instance().Invalidate(address,size);
    auto *memory=Core::Memory::Instance();memory->GetAddressSpace().Register(address,size,backing);
    if(auto *r=memory->GetRasterizer()){r->RegisterMemory(address,size);r->MapMemory(address,size);}
}
extern "C" void iss_gpu_feedback_identity(uint64_t owner_object,uint64_t shader_object,
                                           uint64_t record_address){
    if(FeedbackIdentityEnabled()) {
        iss::game::feedback::IdentityRegistry::Instance().Publish(
            owner_object,shader_object,record_address);
        static std::atomic<uint64_t> events{};
        const auto count=++events;
        if(count<=8||(count&(count-1))==0)
            std::fprintf(stderr,"FB_IDENTITY_EBOOT count=%llu owner=0x%llx shader_object=0x%llx record=0x%llx\n",
                         static_cast<unsigned long long>(count),
                         static_cast<unsigned long long>(owner_object),
                         static_cast<unsigned long long>(shader_object),
                         static_cast<unsigned long long>(record_address));
    }
}
extern "C" int iss_gpu_readback(uint64_t address,uint64_t size){
    if(!size||!host.backing(address,size))return 0;
    auto *renderer=Core::Memory::Instance()->GetRasterizer();if(!renderer)return 0;
    readback_hints.Remember(address);explicit_readback_hints.store(true,std::memory_order_relaxed);
    const auto start=iss::diagnostics::Enabled()?Common::Perf::NowNs():0;
    renderer->ReadMemory(address,size,false,0);
    if(start)iss::diagnostics::RecordReadbackTime(0,address,Common::Perf::NowNs()-start,2);return 1;
}
extern "C" int iss_gpu_invalidate(uint64_t address,uint64_t size){
    // ISS_RUNTIME_WRITE_INVALIDATE=0 leaves file reads to their page faults (one per 4 KB page:
    // the game streams ~5 MB of textures every ~2 s, ~1470 exceptions, part of the periodic
    // 45 ms frames of 2026-10-17).
    static const bool enabled=[]{const char *p=std::getenv("ISS_RUNTIME_WRITE_INVALIDATE");return !p||p[0]!='0';}();
    if(!enabled||size<0x1000||!host.backing(address,size))return 0;
    auto *renderer=Core::Memory::Instance()->GetRasterizer();if(!renderer)return 0;
    return renderer->InvalidateMemory(address,size,false)?1:0;
}
extern "C" int iss_gpu_fault(void *context,void *address){
    if(precise_feedback_site&&!Common::IsWriteError(context)&&
       reinterpret_cast<uintptr_t>(Common::GetRip(context))==host.image_base+0x3f3eff)
        readback_hints.Remember(reinterpret_cast<uintptr_t>(address));
    // Register the hint before ReadMemory schedules its first copy/prefetch.
    if(precise_reduction_site&&!Common::IsWriteError(context)){
        const auto rip=reinterpret_cast<uintptr_t>(Common::GetRip(context));
        if(rip>=host.image_base){const auto offset=rip-host.image_base;
            if(offset==0x1717c0||offset==0x171800)readback_hints.Remember(reinterpret_cast<uintptr_t>(address));}}
    if(iss::repeat_census::enabled)[[unlikely]]
        iss::repeat_census::Note(iss::repeat_census::Fault,reinterpret_cast<uintptr_t>(address)&~uintptr_t{0xfff},0x1000,Common::IsWriteError(context)?1:0);
    const auto start=iss::diagnostics::Enabled()?Common::Perf::NowNs():0;
    const bool handled=Core::Signals::Instance()->DispatchAccessViolation(context,address);
    if(start&&handled)iss::diagnostics::RecordReadbackTime(reinterpret_cast<uintptr_t>(Common::GetRip(context)),reinterpret_cast<uintptr_t>(address),Common::Perf::NowNs()-start,Common::IsWriteError(context)?1:0);
    return handled;
}
// Linux host (gpu_host.cpp): this port's loader names an import "NID#library": the NID alone.
extern "C" uintptr_t iss_gpu_resolve_nid(const char *nid,size_t length){
    for(const auto& s:symbols)if(s.nid.size()==length&&!std::memcmp(s.nid.data(),nid,length))return s.pointer;
    return 0;
}
extern "C" uintptr_t iss_gpu_resolve(const char *identity){
    std::string id(identity);std::vector<std::string> parts;size_t start=0;
    for(size_t end;(end=id.find('#',start))!=std::string::npos;start=end+1)parts.push_back(id.substr(start,end-start));parts.push_back(id.substr(start));
    if(parts.size()!=5||parts[2]!="1")return 0;
    for(const auto& s:symbols)if(s.nid==parts[0]&&s.library==parts[1]&&s.module==parts[3])return s.pointer;
    return 0;
}
// A short sleep that lasts what it asks (a high resolution waitable timer: ~0.5 ms, whatever
// the process timer resolution), for the renderer's polling waits.
extern "C" void IssPreciseSleepUs(unsigned us){
#ifndef _WIN32
    timespec ts{static_cast<time_t>(us/1000000),static_cast<long>(us%1000000)*1000};
    while(nanosleep(&ts,&ts)!=0&&errno==EINTR){}
#else
    thread_local HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    if(!timer){SwitchToThread();return;}
    LARGE_INTEGER due;due.QuadPart=-static_cast<LONGLONG>(us)*10;
    if(SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE))WaitForSingleObject(timer,INFINITE);
    else SwitchToThread();
#endif
}
// An idle wait that sleeps, for one waiting thread (ISS_UPLOAD_IDLE=sleep, the upload worker in
// buffer_cache.cpp): IssIdleWaitFor returns after us microseconds on a high resolution timer, or
// once IssIdleWaitWake is called (a wake while nobody waits ends the next wait at once). Windows
// only (IssIdleWaitCreate returns null elsewhere): libc++ there rounds a timed condition variable
// wait down to whole milliseconds, so a 1 ms wait returns at once and its caller spins; the POSIX
// timed waits sleep.
#ifdef _WIN32
namespace {struct IssIdleWait{HANDLE event,timer;};}
#endif
extern "C" void* IssIdleWaitCreate(){
#ifndef _WIN32
    return nullptr;
#else
    HANDLE timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    if(!timer)return nullptr;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); // auto-reset: no wake is lost
    if(!event){CloseHandle(timer);return nullptr;}
    return new IssIdleWait{event,timer};
#endif
}
extern "C" void IssIdleWaitWake(void* wait){
#ifdef _WIN32
    SetEvent(static_cast<IssIdleWait*>(wait)->event);
#else
    (void)wait;
#endif
}
extern "C" void IssIdleWaitFor(void* wait,unsigned us){
#ifdef _WIN32
    auto* w=static_cast<IssIdleWait*>(wait);
    LARGE_INTEGER due;due.QuadPart=-static_cast<LONGLONG>(us)*10;
    const HANDLE handles[2]={w->event,w->timer};
    if(SetWaitableTimer(w->timer,&due,0,nullptr,nullptr,FALSE))WaitForMultipleObjects(2,handles,FALSE,INFINITE);
    else WaitForSingleObject(w->event,1);
#else
    (void)wait;(void)us;
#endif
}
extern "C" void IssIdleWaitDestroy(void* wait){
#ifdef _WIN32
    if(auto* w=static_cast<IssIdleWait*>(wait)){CloseHandle(w->event);CloseHandle(w->timer);delete w;}
#else
    (void)wait;
#endif
}
// The present thread's vblank clock (ISS_VBLANK_TIMER=hr|hrspin, videoout_driver.cpp): sleeps
// ns on a per-thread timer created once, high resolution when Windows has it (a plain one, about
// 15.6 ms granular, otherwise). Returns 1 when that timer is high resolution; ns<=0 only creates it.
extern "C" int IssVblankSleepNs(long long ns){
#ifndef _WIN32
    if(ns>0){timespec ts{static_cast<time_t>(ns/1000000000),static_cast<long>(ns%1000000000)};
        while(nanosleep(&ts,&ts)!=0&&errno==EINTR){}}
    return 1;
#else
    thread_local int highres=1;
    thread_local HANDLE timer=[]{
        HANDLE t=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
        if(!t){highres=0;t=CreateWaitableTimerW(nullptr,TRUE,nullptr);}
        return t;
    }();
    if(ns<=0)return highres;
    if(!timer){SwitchToThread();return highres;}
    LARGE_INTEGER due;due.QuadPart=-static_cast<LONGLONG>(ns/100);
    if(due.QuadPart==0)due.QuadPart=-1;
    if(SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE))WaitForSingleObject(timer,INFINITE);
    else SwitchToThread();
    return highres;
#endif
}
// The renderer's hooks in the game's code and the checked readback sites: once the image is
// loaded and relocated, before the guest threads start.
extern "C" void iss_gpu_install_image(uint64_t image_base){
    host.image_base=image_base;
    // The eboot's base for the renderer's guest-code lookups: relaxed readback sites
    // (BufferCache::IsRelaxedSite) and the fault-burst attribution (page_manager.cpp).
    MemoryPatcher::g_eboot_address=image_base;
    // The executing image (as native/engine_draw.cpp patches it), not a backing view.
    if(iss::native_shadow::Enabled())
        iss::native_shadow::Install(host.image_base,reinterpret_cast<const unsigned char*>(host.image_base+0x43D590));
    iss::native_gbuffer::InstallCensus(host.image_base);
    iss::native_gbuffer::Install(host.image_base);
    { const auto* c = reinterpret_cast<const unsigned char*>(host.image_base + 0x3f3eff);
      std::fprintf(stderr, "V2_EBOOT us_1_07=%d (renderer v2 sites target CUSA00223 US 1.07)\n", c[0] == 0x8b && c[1] == 0x40 && c[2] == 0x04 && c[3] == 0x49); }
    iss::pause_guard::Install(host.image_base);
    iss::heap_census::Install(host.image_base);
    iss::native_passes::Install(host.image_base);
    if(const char *p=std::getenv("ISS_FEEDBACK_PAGE_READBACK");p&&p[0]=='1'){
        const unsigned char signature[]={0x8b,0x40,0x04,0x49,0x8b,0x8f,0xe8,0,0,0,0x89,0x41,0x08};
        const auto *code=reinterpret_cast<const unsigned char*>(host.image_base+0x3f3eff);
        precise_feedback_site=code&&!std::memcmp(code,signature,sizeof(signature));
        std::fprintf(stderr,"FEEDBACK_READBACK verified=%d window=4096\n",precise_feedback_site);
    }
    if(const char *p=std::getenv("ISS_PRECISE_REDUCTION_READBACK");p&&p[0]=='1'){
        const unsigned char signature[]={0xc4,0xc1,0x7c,0x58,0x45,0xe0,0xc4,0xc1,0x74,0x58,0x4d,0x00};
        const auto *code=reinterpret_cast<const unsigned char*>(host.image_base+0x1717c0);
        precise_reduction_site=code&&!std::memcmp(code,signature,sizeof(signature));
        std::fprintf(stderr,"REDUCTION_READBACK verified=%d window=4096 image_base=0x%llx\n",precise_reduction_site,(unsigned long long)host.image_base);
    }
}
extern "C" int iss_gpu_initialize(const IssGpuCallbacks *callbacks){
    if(!callbacks||callbacks->struct_size!=sizeof(host)||!callbacks->backing||!callbacks->clamp||!callbacks->visit_mappings||!callbacks->protect||!callbacks->mapping_epoch||!callbacks->mapping_lock_shared||!callbacks->mapping_unlock_shared)return -1;
    host=*callbacks;
    iss::timeline::Emit("gpu_initialize",0,0,host.image_base);
    // Linux (this port's loader): the image is known later, iss_gpu_install_image.
    if(host.image_base)iss_gpu_install_image(host.image_base);
    Core::Emulator::Configure();
    for(int i=0;i<=static_cast<int>(Common::FS::PathType::LicensesDir);++i){
        auto type=static_cast<Common::FS::PathType>(i);Common::FS::SetUserPath(type,std::filesystem::path(host.user_dir)/("gpu-"+std::to_string(i)));}
    Common::FS::SetUserPath(Common::FS::PathType::UserDir,host.user_dir);
    // Isolate shader/driver caches for cold/warm comparisons without changing
    // the user's ordinary cache, saves or any other user directory.
    if (const char* cache_root = std::getenv("ISS_PIPELINE_CACHE_ROOT"); cache_root && *cache_root) {
        const auto path = std::filesystem::absolute(std::filesystem::u8path(cache_root));
        std::filesystem::create_directories(path);
        Common::FS::SetUserPath(Common::FS::PathType::CacheDir, path);
        std::fprintf(stderr, "PORT_TUNING pipeline_cache_root=%s\n", path.string().c_str());
    }
    Common::Perf::GetTuning().async_pipelines=0; // Never skip a draw while compiling its pipeline.
    const bool render_reuse=[] {const char *p=std::getenv("ISS_RENDER_REUSE");return p&&p[0]=='1';}();
    auto& tuning=Common::Perf::GetTuning();
    if(const char *p=std::getenv("ISS_PROBE_HOST_IMPORT");p&&p[0]=='1')tuning.probe_host_import=1;
    if(const char *p=std::getenv("ISS_NATIVE_FEEDBACK_PROBE");p&&p[0]=='1')tuning.probe_host_import=1;
    tuning.stream_dedup=render_reuse;tuning.stream_static=render_reuse;
    tuning.sync_memo=render_reuse;tuning.spec_memo=render_reuse;
    tuning.region_summary=render_reuse;tuning.tex_memo=render_reuse;
    tuning.skip_redundant_binds=render_reuse;tuning.shader_params_memo=render_reuse;
    tuning.skip_empty_draws=render_reuse;tuning.pipeline_key_memo=render_reuse;
    tuning.rt_memo=render_reuse;tuning.reuse_buffer_checks=render_reuse;
    const bool descriptor_fast=[] {const char *p=std::getenv("ISS_DESCRIPTOR_FAST_PATH");return p&&p[0]=='1';}();
    tuning.large_read_memo=descriptor_fast;
    std::fprintf(stderr,"PORT_TUNING large_read_memo=%d\n",descriptor_fast);
    const bool occlusion=[] {const char *p=std::getenv("ISS_REAL_OCCLUSION");return p&&p[0]=='1';}();
    tuning.real_occlusion=occlusion;
    const char *readback_setting=std::getenv("ISS_READBACK_WINDOW_KB");
    const long readback_kb=readback_setting?std::strtol(readback_setting,nullptr,10):512;
    tuning.readback_window_bytes=(readback_kb==64||readback_kb==128)?u64(readback_kb)*1024:512*1024;
    std::fprintf(stderr,"PORT_TUNING readback_window_bytes=%llu\n",(unsigned long long)tuning.readback_window_bytes.load());
    const bool parallel_render=[] {const char *p=std::getenv("ISS_PARALLEL_RENDER");return p&&p[0]=='1';}();
    tuning.threaded_recording=parallel_render;tuning.recorder_spin_us=parallel_render?20:0;tuning.readback_queue=parallel_render;
    const bool compute_fairness=[] {const char *p=std::getenv("ISS_COMPUTE_FAIRNESS");return p&&p[0]=='1';}();
    tuning.asc_yield_packets=compute_fairness?16:0;
    const bool async_compute=[] {const char *p=std::getenv("ISS_ASYNC_COMPUTE");return p&&p[0]=='1';}();
    tuning.asc_queue=async_compute;
    std::fprintf(stderr,"PORT_TUNING asc_queue=%d\n",async_compute);
    std::fprintf(stderr,"PORT_TUNING asc_yield_packets=%u\n",compute_fairness?16:0);
    std::fprintf(stderr,"PORT_TUNING threaded_recording=%d readback_queue=%d recorder_spin_us=%u\n",parallel_render,parallel_render,parallel_render?20:0);
    std::fprintf(stderr,"PORT_TUNING render_reuse=%d real_occlusion=%d\n",render_reuse,occlusion);
    const bool upload_worker=[] {const char *p=std::getenv("ISS_UPLOAD_WORKER");return p&&p[0]=='1';}();
    Common::Perf::GetTuning().async_uploads=upload_worker;
    std::fprintf(stderr,"PORT_TUNING async_uploads=%d async_pipelines=0\n",upload_worker);
    if(const char *p=std::getenv("ISS_GPU_PROFILE");p&&(p[0]=='1'||p[0]=='2'))Common::Perf::GetTuning().gpu_profile=p[0]-'0';
    {const char *p=std::getenv("ISS_COMPUTE_PHASES");iss::phase::g_enabled=p&&p[0]=='1';
     std::fprintf(stderr,"PORT_TUNING compute_phases=%d\n",iss::phase::Enabled()?1:0);}
    // ISS_TIMER_1MS=1 (off by default, the user's choice): 1 ms timer resolution for this
    // process. Without it a timed wait of 1-2 ms (the game's condition variables, event flags and
    // semaphores with a timeout, every Sleep(1)) lasts ~15.6 ms on Windows 11 (measured Sleep(1) =
    // 15.1 ms). Scripted city A/B (ab2, 2026-10-18 night, 2 runs each): no measurable difference.
#ifdef _WIN32
    if(const char *p=std::getenv("ISS_TIMER_1MS");p&&p[0]=='1'){
        if(HMODULE winmm=LoadLibraryW(L"winmm.dll")){
            using TimeBeginPeriod=UINT(WINAPI*)(UINT);
            if(auto begin=reinterpret_cast<TimeBeginPeriod>(GetProcAddress(winmm,"timeBeginPeriod"))){
                const UINT result=begin(1);
                std::fprintf(stderr,"PORT_TUNING timer_1ms=%d\n",result==0?1:0);
            }
        }
    }
    if(thread_priority_enabled()){
        const BOOL ok=SetPriorityClass(GetCurrentProcess(),ABOVE_NORMAL_PRIORITY_CLASS);
        std::fprintf(stderr,"PORT_TUNING process_priority=above_normal ok=%d\n",ok?1:0);
    }
    iss_set_high_qos(false);
#endif
    // ISS_TUNING="name=value,...": existing renderer toggles for A/B tests, applied last.
    if(const char *p=std::getenv("ISS_TUNING")){
        const auto set=[](auto& field,const std::string& value){field=static_cast<std::remove_reference_t<decltype(field.load())>>(std::strtoull(value.c_str(),nullptr,0));};
        std::string all=p;size_t at=0;
        while(at<all.size()){
            size_t end=all.find(',',at);if(end==std::string::npos)end=all.size();
            const std::string item=all.substr(at,end-at);at=end+1;
            const size_t eq=item.find('=');if(eq==std::string::npos)continue;
            const std::string name=item.substr(0,eq),value=item.substr(eq+1);bool known=true;
            if(name=="late_protect")set(tuning.late_protect,value);
            else if(name=="parallel_protect")set(tuning.parallel_protect,value);
            else if(name=="bind_prefetch")set(tuning.bind_prefetch,value);
            else if(name=="scout")set(tuning.scout,value);
            else if(name=="scout_fast")set(tuning.scout_fast,value);
            else if(name=="hot_page_faults")set(tuning.hot_page_faults,value);
            else if(name=="async_written_uploads")set(tuning.async_written_uploads,value);
            else if(name=="async_uploads")set(tuning.async_uploads,value);
            else if(name=="readback_write_discard")set(tuning.readback_write_discard,value);
            else if(name=="readback_write_merge")set(tuning.readback_write_merge,value);
            else if(name=="gpu_threads_priority")set(tuning.gpu_threads_priority,value);
            else if(name=="asc_yield_packets")set(tuning.asc_yield_packets,value);
            else if(name=="recorder_spin_us")set(tuning.recorder_spin_us,value);
            else if(name=="tiered_compute_pipelines")set(tuning.tiered_compute_pipelines,value);
            else if(name=="max_gpu_lag_frames")set(tuning.max_gpu_lag_frames,value);
            else if(name=="gpu_sync_frame_end")set(tuning.gpu_sync_frame_end,value);
            // relaxed_site=<eboot offset> (repeatable, up to 8): reads of GPU written memory from
            // that 256 byte block of the game's code do not wait for the GPU (readback_relaxed_sites).
            else if(name=="relaxed_site"){const u32 n=tuning.num_relaxed_sites.load();if(n<Common::Perf::Tuning::MaxRelaxedSites){tuning.relaxed_sites[n]=std::strtoull(value.c_str(),nullptr,0)&~0xFFull;tuning.num_relaxed_sites.store(n+1);}}
            else if(name=="relaxed_all")tuning.relaxed_all=std::strtoull(value.c_str(),nullptr,0)!=0;
            else known=false;
            std::fprintf(stderr,"PORT_TUNING %s=%s%s\n",name.c_str(),value.c_str(),known?"":" (unknown, ignored)");
        }
    }
    window_thread=std::thread([]{
        try{
            g_window=new Frontend::WindowSDL(host.width,host.height,host.title);
            // The window answers from its first instant: the libraries, the renderer and the
            // shader preload start on their own thread while this one pumps the window's
            // messages (they ran here first: the window did not answer for 8 s, 30 s with the
            // launcher's shader cache, and Windows marked it "not responding"; winprobe1
            // 2026-10-18). The title shows the preload's progress meanwhile.
            static Core::Loader::SymbolsResolver resolver;
            std::atomic<int> init_state{0}; // 0 running, 1 done, -1 failed
            std::string init_error;
            std::thread init([&]{
                try{
                    Libraries::Kernel::RegisterEventQueue(&resolver);
                    Libraries::Kernel::work.emplace(boost::asio::make_work_guard(Libraries::Kernel::io_context));
                    Libraries::Kernel::service_thread=std::thread([]{Libraries::Kernel::io_context.run();});Libraries::Kernel::service_thread.detach();
                    Libraries::GnmDriver::RegisterLib(&resolver);Libraries::VideoOut::RegisterLib(&resolver);
                    // Linux host: this port's first-start job file (gpu/precompile_jobs.inl).
                    if(auto *r=Core::Memory::Instance()->GetRasterizer()){
                        iss::g_preload_phase.store(6,std::memory_order_relaxed);
                        // This port's "Compiling shaders" screen (tlg_overlay.cpp), presented
                        // from here: the game has not started drawing.
                        std::fprintf(stderr,"PRECOMPILE_JOBS start: imgui context %p\n",(void*)ImGui::GetCurrentContext());
                        TlgOverlay::SetLoadingCompile(true);
                        TlgOverlay::SetLoading(true,0,0);
                        // The screen is drawn by a thread of its own, every 250 ms, however
                        // long one record takes (the present scheduler: not the draw one the
                        // lookups use).
                        std::atomic<bool> loading{true};
                        std::thread painter([&loading]{
                            Common::SetCurrentThreadName("iss:LoadingScreen");
                            const auto started=std::chrono::steady_clock::now();
                            bool shot=false;
                            static const bool want_shot=[]{const char *p=std::getenv("ISS_LOADING_SHOT");return p&&p[0]=='1';}();
                            while(loading.load(std::memory_order_acquire)){
                                if(want_shot&&!shot&&std::chrono::steady_clock::now()-started>std::chrono::seconds(4)){
                                    shot=true;VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::WithOverlays);}
                                // (Presented by the renderer's own present thread: it draws the overlay.)
                                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                            }
                        });
                        r->GetPipelineCache().PrecompileJobs([](u32 done,u32 total){
                            iss::g_preload_total.store(total,std::memory_order_relaxed);
                            iss::g_preload_entries.store(done,std::memory_order_relaxed);
                            TlgOverlay::SetLoading(true,done,total);});
                        loading.store(false,std::memory_order_release);
                        painter.join();
                        TlgOverlay::SetLoadingCompile(false);
                        TlgOverlay::SetLoading(false,0,0);
                        iss::g_preload_phase.store(3,std::memory_order_relaxed);
                    }
                    init_state.store(1,std::memory_order_release);
                }catch(const std::exception& e){init_error=e.what();init_state.store(-1,std::memory_order_release);}
            });
            {
                auto last_title=std::chrono::steady_clock::now()-std::chrono::seconds(1);
                std::string shown;
                while(init_state.load(std::memory_order_acquire)==0){
                    if(!g_window->PollEvents()){
                        std::fputs("PORT_STOP window closed while loading\n",stderr);std::fflush(nullptr);
                        IssHostExit();
                    }
                    const auto now=std::chrono::steady_clock::now();
                    if(now-last_title>std::chrono::milliseconds(40)){
                        last_title=now;
                        const unsigned entries=iss::g_preload_entries.load(std::memory_order_relaxed);
                        const unsigned total=iss::g_preload_total.load(std::memory_order_relaxed);
                        const int phase=iss::g_preload_phase.load(std::memory_order_relaxed);
                        // Phase 4: the first run compiles the game's compute shaders (ISS_AOT_WAIT).
                        std::string status=phase==0?"Starting...":
                            phase==1?"Loading shaders  "+std::to_string(entries)+(total?" / "+std::to_string(total):""):
                            phase==4?"Preparing shaders (first run only)  "+std::to_string(entries)+" / "+std::to_string(total):
                            phase==5?"Indexing the game's shaders (first run only)  "+std::to_string(entries)+" / "+std::to_string(total):
                            phase==6?"Compiling shaders (first run only)  "+std::to_string(entries)+" / "+std::to_string(total):
                            "Finishing shaders...";
                        // Phase 6 is this port's own "Compiling shaders" screen (the overlay,
                        // presented by the renderer): the window's screen stays off then.
                        if(phase!=6)g_window->DrawLoading(phase,entries,phase==1||phase==4||phase==5?total:0);
                        std::string title=std::string(host.title)+" - "+status;
                        if(title!=shown){SDL_SetWindowTitle(g_window->GetSDLWindow(),title.c_str());shown=title;}
                    }
                    // Not sleep_for: without a raised timer period it lasts a ~15.6 ms tick.
                    IssPreciseSleepUs(5000);
                }
                init.join();
                SDL_SetWindowTitle(g_window->GetSDLWindow(),host.title);
                if(init_state.load()<0)throw std::runtime_error(init_error);
            }
            auto *renderer=Core::Memory::Instance()->GetRasterizer();
            const bool checkpoints=[] {const char *v=std::getenv("ISS_PIPELINE_CHECKPOINT");return !v||v[0]!='0';}();
            if(renderer&&checkpoints)renderer->GetPipelineCache().CheckpointDriverCache();
            {std::scoped_lock guard{ready_mutex};ready=1;}ready_cv.notify_all();
            const char *stop_path=std::getenv("ISS_STOP_FILE");
            auto last_checkpoint=std::chrono::steady_clock::now(),last_stop_check=last_checkpoint;
            auto last_priority=last_checkpoint-std::chrono::seconds(8);
            for(;;){
                bool stop=!g_window->PollEvents();auto now=std::chrono::steady_clock::now();
                if(stop_path&&now-last_stop_check>std::chrono::milliseconds(100)){std::error_code ec;stop|=std::filesystem::exists(stop_path,ec);last_stop_check=now;}
                stop|=IssHostStopRequested();
                if(stop){
                    if(renderer&&checkpoints)renderer->GetPipelineCache().CheckpointDriverCache(true);
                    // Our atomic checkpoint replaces the old destructor-only snapshot.
                    Storage::DataBase::Instance().SetBeforeClose({});Storage::DataBase::Instance().Close();
                    std::fputs("PORT_STOP cache writes drained; closing all guest threads and audio\n",stderr);std::fflush(nullptr);
                    IssHostExit();return;
                }
                if(thread_priority_enabled()&&now-last_priority>std::chrono::seconds(10)){raise_gpu_threads();last_priority=now;}
                // Off the window thread: a changed driver cache takes ~120 ms to write.
                if(checkpoints&&renderer&&now-last_checkpoint>std::chrono::seconds(30)){
                    std::thread([renderer]{renderer->GetPipelineCache().CheckpointDriverCache();}).detach();last_checkpoint=now;}
                // Not sleep_for: SDL3 sets no timer period, so a 2 ms sleep_for (Sleep(2)) lasts
                // a ~15.6 ms tick and input is pumped at ~64 Hz. The high resolution timer sleeps
                // ~2 ms.
                IssPreciseSleepUs(2000);
            }
        }catch(const std::exception& e){std::fprintf(stderr,"GPU initialization failed: %s\n",e.what());{std::scoped_lock guard{ready_mutex};ready=-1;}ready_cv.notify_all();}
    });window_thread.detach();
    std::unique_lock guard{ready_mutex};ready_cv.wait(guard,[]{return ready!=0;});return ready==1?0:-1;
}

uint64_t IssImageBase() { return host.image_base; }

// ISS_CP_CPU: the command processor thread limits the frame rate while several guest threads
// spin on rdtsc waiting for it, so it gets a physical core of its own.
//   auto           Windows: the last physical core of the fastest class (the last P-core of a
//                  hybrid CPU such as the i7-13700H: LP 10/11), never core 0. On a non-hybrid CPU
//                  the last core sharing the L3 of LP 0 (LP 14/15 of an 8-core/16-thread CPU, the
//                  V-cache CCD of a 7950X3D), or the last core overall if the cache query fails.
//                  With fewer than 4 physical cores nothing is pinned.
//                  Linux: on a hybrid Intel CPU (/sys/devices/cpu_core/cpus lists the P-cores'
//                  logical processors) the first logical processor of the last P-core;
//                  otherwise the same as 14.
//   <L>            the physical core of logical processor L (a number: the original behaviour).
//   empty, off, none, -1   no pinning.
//   anything else  a warning, then auto.
// Pinned: the core is taken out of the process default CPU sets (no other thread of the process
// runs there, its SMT sibling stays idle) and the command processor gets one logical processor
// of it alone (L, or the core's first one for auto). Only processor group 0 is used.
namespace {
enum class CpMode { Off, Auto, Manual };
struct CpChoice { CpMode mode; long lp; };
CpChoice cp_mode(const char* value) {
    if (!value) return {CpMode::Off, -1};
    std::string v = value;
    const auto space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!v.empty() && space(v.back())) v.pop_back();
    size_t first = 0;
    while (first < v.size() && space(v[first])) first++;
    v.erase(0, first);
    for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v.empty() || v == "off" || v == "none" || v == "-1") return {CpMode::Off, -1};
    if (v == "auto") return {CpMode::Auto, -1};
    errno = 0;
    char* end = nullptr;
    const long lp = std::strtol(v.c_str(), &end, 10);
    if (std::isdigit(static_cast<unsigned char>(v[0])) && end && *end == '\0' && errno == 0 && lp >= 0 && lp < 4096) {
        return {CpMode::Manual, lp};
    }
    std::fprintf(stderr, "PORT_TUNING cp_cpu warning: unrecognised ISS_CP_CPU=\"%s\": using auto\n", v.c_str());
    return {CpMode::Auto, -1};
}
} // namespace

#ifndef _WIN32
// Linux: the command processor thread alone on logical processor L (the other threads keep
// their affinity: CPU sets have no Linux equivalent that excludes a core from a process).
// `auto`: the last P-core of a hybrid Intel CPU, else the old default 14 (or the last logical
// processor).
namespace {
// The highest logical processor of a sysfs CPU list ("0-11", "0-5,12-15", "3"), -1 if none.
long last_in_cpu_list(const char* path) {
    FILE* file = std::fopen(path, "r");
    if (!file) return -1;
    char text[512]{};
    const bool read = std::fgets(text, sizeof(text), file) != nullptr;
    std::fclose(file);
    if (!read) return -1;
    long last = -1;
    for (const char* p = text; *p;) {
        if (!std::isdigit(static_cast<unsigned char>(*p))) {
            ++p;
            continue;
        }
        char* end = nullptr;
        const long value = std::strtol(p, &end, 10);
        last = std::max(last, value);
        p = end;
    }
    return last;
}
// The first logical processor of a sysfs CPU list, -1 if none.
long first_in_cpu_list(const char* path) {
    FILE* file = std::fopen(path, "r");
    if (!file) return -1;
    long first = -1;
    if (std::fscanf(file, "%ld", &first) != 1) first = -1;
    std::fclose(file);
    return first;
}
// Hybrid Intel CPU: the first logical processor of the last P-core (as Windows' auto picks it:
// LP 10 of an i7-13700H), -1 when the kernel lists no P-cores.
long last_p_core_lp() {
    const long last = last_in_cpu_list("/sys/devices/cpu_core/cpus");
    if (last < 0) return -1;
    char siblings[128];
    std::snprintf(siblings, sizeof(siblings),
                  "/sys/devices/system/cpu/cpu%ld/topology/thread_siblings_list", last);
    const long first = first_in_cpu_list(siblings);
    return first >= 0 && first <= last ? first : last;
}
} // namespace
void IssPinCommandProcessor() {
    const CpChoice choice = cp_mode(std::getenv("ISS_CP_CPU"));
    if (choice.mode == CpMode::Off) return;
    const long p_core = choice.mode == CpMode::Auto ? last_p_core_lp() : -1;
    long wanted = choice.mode == CpMode::Auto ? (p_core >= 0 ? p_core : 14) : choice.lp;
    const long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count > 0 && wanted >= count) wanted = count - 1; // fewer logical processors than asked
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<int>(wanted), &set);
    const int ok = pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
    std::fprintf(stderr, "PORT_TUNING cp_cpu=%ld mode=%s thread=%d\n", wanted,
                 choice.mode == CpMode::Auto ? (p_core >= 0 ? "auto(linux: last p-core)"
                                                            : "auto(linux: 14)")
                                             : "manual",
                 ok);
}
#else


static const char* cp_mode_name(CpMode mode) {
    return mode == CpMode::Auto ? "auto" : mode == CpMode::Manual ? "manual" : "off";
}

namespace {
// The command processor's placement (ISS_CP_CPU), chosen once for the process: the topology and
// the choice are logged here. pinned: the core it gets (core_ids: every CPU set of that core;
// id: the one of them the command processor thread runs on); not pinned: nothing else is set.
struct CpSelection {
    bool pinned{};
    CpMode mode{CpMode::Off};
    long wanted{-1};
    ULONG id{};
    unsigned logical{}, core{}, efficiency{};
    bool hybrid{};
    const char* scope{""};
    std::vector<ULONG> others;   // every CPU set but the core's: the process default
    std::vector<ULONG> core_ids; // the core's CPU sets
    std::string siblings;        // the core's logical processors, "10/11"
};

CpSelection SelectCommandProcessorCpu() {
    CpSelection sel;
    const char* value = std::getenv("ISS_CP_CPU");
    const CpChoice choice = cp_mode(value);
    const CpMode mode = choice.mode;
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    std::vector<char> buffer(length);
    if (!length || !GetSystemCpuSetInformation(
            reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), length, &length,
            GetCurrentProcess(), 0)) {
        std::fprintf(stderr, "PORT_TUNING cp_cpu failed: no CPU set information\n");
        return sel;
    }
    struct Set { ULONG id; unsigned core; unsigned logical; unsigned group; unsigned efficiency; };
    std::vector<Set> sets;
    for (size_t offset = 0; offset < length;) {
        const auto* info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
        if (info->Type == CpuSetInformation) {
            sets.push_back({info->CpuSet.Id, info->CpuSet.CoreIndex,
                            info->CpuSet.LogicalProcessorIndex, info->CpuSet.Group,
                            info->CpuSet.EfficiencyClass});
        }
        offset += info->Size;
    }
    // Physical cores: the logical processors with the same (Group, CoreIndex), SMT siblings.
    struct Core { unsigned group, core, efficiency; std::vector<const Set*> lps; };
    std::vector<Core> cores;
    for (const auto& set : sets) {
        auto it = std::find_if(cores.begin(), cores.end(),
                               [&](const Core& c) { return c.group == set.group && c.core == set.core; });
        if (it == cores.end()) {
            cores.push_back({set.group, set.core, set.efficiency, {}});
            it = cores.end() - 1;
        }
        it->lps.push_back(&set);
    }
    for (auto& core : cores) {
        std::sort(core.lps.begin(), core.lps.end(), [](const Set* a, const Set* b) { return a->logical < b->logical; });
    }
    std::map<unsigned, std::pair<unsigned, unsigned>> classes; // efficiency class -> cores, LPs
    for (const auto& core : cores) {
        auto& entry = classes[core.efficiency];
        entry.first++;
        entry.second += static_cast<unsigned>(core.lps.size());
    }
    const bool hybrid = classes.size() > 1;
    const unsigned top_class = classes.empty() ? 0 : classes.rbegin()->first;
    {
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::string text;
            for (auto it = classes.rbegin(); it != classes.rend(); ++it) {
                char item[96];
                const char* name = !hybrid ? "all" : it->first == top_class ? "P" : "E";
                std::snprintf(item, sizeof(item), "%s%s %uc/%ut class %u", text.empty() ? "" : ", ", name,
                              it->second.first, it->second.second, it->first);
                text += item;
            }
            std::fprintf(stderr, "PLACEMENT topology: %zu LPs/%zu cores, hybrid=%d, %s\n", sets.size(),
                         cores.size(), hybrid ? 1 : 0, text.c_str());
        }
    }
    if (mode == CpMode::Off) {
        std::fprintf(stderr, "PORT_TUNING cp_cpu=%s mode=off hybrid=%d\n", value && *value ? value : "(empty)",
                     hybrid ? 1 : 0);
        return sel;
    }
    const Set* mine = nullptr;
    long wanted = -1;
    const char* scope = "";
    if (mode == CpMode::Auto) {
        size_t group0_cores = 0;
        for (const auto& core : cores) group0_cores += core.group == 0;
        if (group0_cores < 4) { // a 2- or 3-core CPU keeps every core for the game's threads
            std::fprintf(stderr, "PORT_TUNING cp_cpu=auto mode=auto not pinned: %zu physical cores in group 0 "
                         "(fewer than 4)\n", group0_cores);
            return sel;
        }
        // Non-hybrid: the group 0 logical processors sharing the L3 of LP 0 (one CCD of a
        // dual-CCD Ryzen, where the game's threads run), 0 if unknown.
        KAFFINITY l3_mask = 0;
        if (!hybrid) {
            DWORD cache_length = 0;
            GetLogicalProcessorInformationEx(RelationCache, nullptr, &cache_length);
            std::vector<char> cache(cache_length);
            if (cache_length && GetLogicalProcessorInformationEx(
                    RelationCache, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(cache.data()),
                    &cache_length)) {
                for (DWORD offset = 0; offset < cache_length && !l3_mask;) {
                    const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(cache.data() + offset);
                    if (!info->Size) break;
                    if (info->Relationship == RelationCache && info->Cache.Level == 3) {
                        const WORD groups = info->Cache.GroupCount ? info->Cache.GroupCount : 1;
                        for (WORD g = 0; g < groups; g++) {
                            const GROUP_AFFINITY& mask = info->Cache.GroupMasks[g];
                            if (mask.Group == 0 && (mask.Mask & 1)) l3_mask = mask.Mask;
                        }
                    }
                    offset += info->Size;
                }
            }
            if (!l3_mask) std::fprintf(stderr, "PORT_TUNING cp_cpu=auto: no L3 information, using the last core\n");
        }
        // The last core of group 0 in the fastest class (by its first logical processor), not
        // core 0, and on a non-hybrid CPU with every logical processor in the L3 of LP 0.
        const auto in_l3 = [&](const Core& core) {
            if (!l3_mask) return true;
            for (const Set* lp : core.lps) {
                if (lp->logical >= 64 || !(l3_mask & (KAFFINITY(1) << lp->logical))) return false;
            }
            return true;
        };
        const Core* pick = nullptr;
        for (const auto& core : cores) {
            if (core.group != 0 || core.efficiency != top_class || core.lps.front()->logical == 0) continue;
            if (!in_l3(core)) continue;
            if (!pick || core.lps.front()->logical > pick->lps.front()->logical) pick = &core;
        }
        if (!pick) {
            std::fprintf(stderr, "PORT_TUNING cp_cpu=auto mode=auto failed: no core besides core 0; not pinned\n");
            return sel;
        }
        if (l3_mask) scope = " scope=l3(lp0)";
        mine = pick->lps.front();
        wanted = mine->logical;
    } else {
        wanted = choice.lp;
        for (const auto& set : sets) {
            if (set.group == 0 && set.logical == wanted) mine = &set;
        }
        // A CPU with fewer logical processors than asked (ISS_CP_CPU=14 on a 6-core/12-thread CPU):
        // the last one of group 0, so the command processor still gets a core of its own.
        if (!mine) {
            for (const auto& set : sets) {
                if (set.group == 0 && (!mine || set.logical > mine->logical)) mine = &set;
            }
            if (mine) std::fprintf(stderr, "PORT_TUNING cp_cpu %ld not found: using %u\n", wanted, unsigned(mine->logical));
        }
        if (!mine) {
            std::fprintf(stderr, "PORT_TUNING cp_cpu failed: logical processor %ld not found\n", wanted);
            return sel;
        }
    }
    sel.pinned = true;
    sel.mode = mode;
    sel.wanted = wanted;
    sel.id = mine->id;
    sel.logical = unsigned(mine->logical);
    sel.core = unsigned(mine->core);
    sel.efficiency = unsigned(mine->efficiency);
    sel.hybrid = hybrid;
    sel.scope = scope;
    for (const auto& set : sets) {
        if (set.group != mine->group || set.core != mine->core) {
            sel.others.push_back(set.id);
        } else {
            sel.core_ids.push_back(set.id);
            sel.siblings += (sel.siblings.empty() ? "" : "/") + std::to_string(set.logical);
        }
    }
    return sel;
}

const CpSelection& CommandProcessorCpu() {
    static const CpSelection selection = SelectCommandProcessorCpu(); // thread-safe, once
    return selection;
}
} // namespace

// The CPU sets of the command processor's core (ISS_CP_CPU), whichever thread asks first; false
// (out empty) when it is not pinned. native_passes.cpp keeps its pool off them.
bool IssCommandProcessorCpuSets(std::vector<ULONG>& out) {
    const CpSelection& sel = CommandProcessorCpu();
    out = sel.pinned ? sel.core_ids : std::vector<ULONG>{};
    return !out.empty();
}

void IssPinCommandProcessor() {
    iss_set_high_qos(true);
    const CpSelection& sel = CommandProcessorCpu();
    if (!sel.pinned) return;
    const BOOL process_ok = !sel.others.empty() &&
        SetProcessDefaultCpuSets(GetCurrentProcess(), sel.others.data(), static_cast<ULONG>(sel.others.size()));
    const ULONG id = sel.id;
    const BOOL thread_ok = SetThreadSelectedCpuSets(GetCurrentThread(), &id, 1);
    std::fprintf(stderr,
                 "PORT_TUNING cp_cpu=%s mode=%s lp=%u core=%u lps=%s class=%u hybrid=%d%s "
                 "process_default=%d(%zu sets) thread=%d\n",
                 sel.mode == CpMode::Auto ? "auto" : std::to_string(sel.wanted).c_str(), cp_mode_name(sel.mode),
                 sel.logical, sel.core, sel.siblings.c_str(), sel.efficiency,
                 sel.hybrid ? 1 : 0, sel.scope, int(process_ok), sel.others.size(), int(thread_ok));
}
#endif
