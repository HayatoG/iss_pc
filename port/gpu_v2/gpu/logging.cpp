// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <fmt/format.h>
#include "common/logging/log.h"
namespace Common::Log {
// Warning for every class; ISS_LOG_LEVEL=trace|debug|info|warning|error|critical|off sets them all
// (info: the renderer's Info messages, as before).
std::array<Level,NUM_LOG_CLASSES> g_class_levels=[] {
    Level level=Level::Warning;
    if(const char *env=std::getenv("ISS_LOG_LEVEL")){
        std::string name(env);
        for(char &ch:name)ch=char(std::tolower((unsigned char)ch));
        constexpr const char *names[]={"trace","debug","info","warning","error","critical","off"};
        for(unsigned i=0;i<std::size(names);++i)if(name==names[i])level=Level(i);
    }
    std::array<Level,NUM_LOG_CLASSES> a;a.fill(level);return a;
}();
void VLog(Class c,Level level,const char *file,int line,const char *function,fmt::string_view format,fmt::format_args args){
    // Counted by call site, not by text (a message carrying an address or a size would always be
    // new): printed the first time and at powers of two, formatted only then. The hot paths that
    // log every draw cost an atomic increment, not a lock and two formats.
    static std::array<std::atomic<unsigned long long>,4096> counts{};
    const unsigned long long site=(reinterpret_cast<std::uintptr_t>(file)*0x9E3779B97F4A7C15ull)^
        (unsigned long long)(line)<<12^(unsigned long long)(level)<<8^(unsigned long long)(c);
    const unsigned long long count=counts[(site^site>>29)%counts.size()].fetch_add(1,std::memory_order_relaxed)+1;
    if(count&(count-1))return;
    const auto text=fmt::vformat(format,args);
    static std::mutex mutex;std::scoped_lock guard{mutex};
    if(count>1)std::fprintf(stderr,"GPU_REPEAT count=%llu ",count);
    std::fprintf(stderr,"GPU [%u:%u] %s:%d %s: %s\n",unsigned(c),unsigned(level),file,line,function,text.c_str());
}
void Flush(){std::fflush(stderr);}
void Shutdown(){Flush();}
void Terminate(){Flush();}
}
