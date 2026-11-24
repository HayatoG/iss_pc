// SPDX-License-Identifier: GPL-2.0-or-later
#include "trophy_store.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <set>
#include <stdexcept>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif
namespace IssTrophies {
namespace {
uint64_t Be(const std::vector<unsigned char>& b,size_t p,int n){
    if(p>b.size()||size_t(n)>b.size()-p)throw std::runtime_error("Truncated trophy archive");
    uint64_t v=0;while(n--)v=(v<<8)|b[p++];return v;
}
void Regular(const std::filesystem::path& p){
    if(std::filesystem::is_symlink(p))throw std::runtime_error("Linked trophy storage refused");
}
}
std::vector<Entry> ParseTrp(const std::vector<unsigned char>& b){
    if(b.size()<96||b.size()>32*1024*1024||Be(b,0,4)!=0xdca24d00||Be(b,4,4)!=3||Be(b,8,8)!=b.size())
        throw std::runtime_error("Unsupported or damaged trophy archive");
    size_t count=Be(b,16,4),stride=Be(b,20,4);
    if(!count||count>1024||stride!=64||count>(b.size()-96)/64)throw std::runtime_error("Invalid trophy entry table");
    std::vector<Entry> result;std::set<std::string> names;std::vector<std::pair<size_t,size_t>> spans;
    for(size_t i=0;i<count;i++){
        size_t p=96+i*64,n=0;while(n<32&&b[p+n])n++;
        std::string name(reinterpret_cast<const char*>(b.data()+p),n);
        if(n==0||n==32||name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.")!=std::string::npos||name.find("..")!=std::string::npos||!names.insert(name).second)
            throw std::runtime_error("Unsafe or duplicate trophy entry name");
        uint64_t offset=Be(b,p+32,8),size=Be(b,p+40,8);auto flags=uint32_t(Be(b,p+48,4));
        if(offset<96+count*64||offset>b.size()||size>b.size()-offset||!size||size>2*1024*1024)
            throw std::runtime_error("Trophy payload outside archive");
        for(auto [low,high]:spans)if(offset<high&&offset+size>low)throw std::runtime_error("Overlapping trophy entries");
        spans.emplace_back(offset,offset+size);
        result.push_back({name,{b.begin()+offset,b.begin()+offset+size},flags});
    }return result;
}
std::string Fingerprint(const std::vector<unsigned char>& b){
    // Identity checksum, not a security signature or authenticity check.
    uint64_t hash=14695981039346656037ull;for(auto c:b){hash^=c;hash*=1099511628211ull;}
    char out[17];std::snprintf(out,sizeof(out),"%016llx",(unsigned long long)hash);return out;
}
Progress Load(const std::filesystem::path& path,const std::string& fingerprint,int total,const std::string& save_identity,
              const std::string& title_id){
    Regular(path);Progress out{fingerprint,{},save_identity,{},title_id};if(!std::filesystem::exists(path))return out;
    if(std::filesystem::file_size(path)>65536)throw std::runtime_error("Oversized trophy ledger");
    std::ifstream input(path);auto j=nlohmann::json::parse(input);
    if(j.at("schema")!=1||j.at("title_id")!=title_id||j.at("package_fingerprint")!=fingerprint)throw std::runtime_error("Trophy ledger identity mismatch");
    if(j.value("save_identity",std::string{})!=save_identity)throw std::runtime_error("Trophy save identity mismatch");
    auto rows=j.at("earned");if(!rows.is_array()||rows.size()>128)throw std::runtime_error("Invalid trophy ledger");
    for(auto& row:rows){int id=row.at("id").get<int>();int64_t timestamp=row.at("time").get<int64_t>();
        if(id<0||id>=total||timestamp<=0||!out.earned.emplace(id,timestamp).second)throw std::runtime_error("Invalid trophy record");}
    for(auto& value:j.value("recovered",nlohmann::json::array())){int id=value.get<int>();if(!out.earned.contains(id)||!out.recovered.insert(id).second)throw std::runtime_error("Invalid recovered trophy provenance");}
    return out;
}
void Save(const std::filesystem::path& path,const Progress& p){
    Regular(path);Regular(path.parent_path());std::filesystem::create_directories(path.parent_path());
    auto temp=path;temp+=".tmp";Regular(temp);auto backup=path;backup+=".bak";Regular(backup);
    nlohmann::json rows=nlohmann::json::array();for(auto [id,time]:p.earned)rows.push_back({{"id",id},{"time",time}});
    nlohmann::json j={{"schema",1},{"title_id",p.title_id},{"package_fingerprint",p.fingerprint},{"save_identity",p.save_identity},{"earned",rows},{"recovered",p.recovered}};
    {std::ofstream file(temp,std::ios::binary|std::ios::trunc);file<<j.dump(2)<<'\n';file.flush();if(!file)throw std::runtime_error("Could not write trophy ledger");}
    if(std::filesystem::exists(path))std::filesystem::copy_file(path,backup,std::filesystem::copy_options::overwrite_existing);
#ifdef _WIN32
    if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Could not commit trophy ledger");
#else
    std::filesystem::rename(temp,path);
#endif
}
}
