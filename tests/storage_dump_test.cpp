#include "../src/DumpValidation.h"
#include "../src/StoragePolicy.h"
#include <cassert>
#include <iostream>
namespace fs = std::filesystem;
static void Put(std::vector<char>& b, std::size_t at, std::uint32_t n) {
    for (int i=0; i<4; ++i) b[at+i] = static_cast<char>((n >> (i*8)) & 255);
}
static void Save(const fs::path& p, const std::vector<char>& b) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary); f.write(b.data(), b.size());
}
int main() {
    const auto root = fs::temp_directory_path() / ("watcher_policy_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    std::vector<char> dump(304);
    Put(dump,0,0x504d444d); Put(dump,4,0xa793); Put(dump,8,3); Put(dump,12,32);
    Put(dump,32,3); Put(dump,36,52); Put(dump,40,68);
    Put(dump,44,4); Put(dump,48,112); Put(dump,52,120);
    Put(dump,56,7); Put(dump,60,56); Put(dump,64,232);
    Put(dump,68,1); Put(dump,120,1);
    Put(dump,112,16); Put(dump,116,288);
    const auto file = root / "test.dmp";
    Save(file,dump); std::string error;
    assert(DumpValidation::Validate(file,error));
    auto bad=dump;
    std::fill(bad.begin()+32,bad.begin()+68,0); Save(file,bad);
    assert(!DumpValidation::Validate(file,error)); // reported zero stream directory
    bad=dump; Put(bad,12,0xfffffff0); Save(file,bad);
    assert(!DumpValidation::Validate(file,error)); // directory outside file
    bad=dump; Put(bad,116,300); Save(file,bad);
    assert(!DumpValidation::Validate(file,error)); // nested context payload truncated
    bad=dump; bad.resize(288); Save(file,bad);
    assert(!DumpValidation::Validate(file,error)); // interrupted write
    bad=dump; Put(bad,60,12); Save(file,bad);
    assert(!DumpValidation::Validate(file,error)); // short required system stream
    // Full-memory directory payload must fit, including sizes above 4 GiB.
    bad=dump; Put(bad,56,9); Put(bad,60,32); Put(bad,64,232);
    Put(bad,232,1); Put(bad,240,288); Put(bad,256,0xffffffff); Save(file,bad);
    assert(!DumpValidation::Validate(file,error));
    fs::remove(file);
    const auto old=fs::file_time_type::clock::now()-std::chrono::hours(1);
    const auto a=root/"old"/"first.dmp", b=root/"new"/"second.dmp", text=root/"events_old.log", live=root/"events_live.log";
    Save(a,std::vector<char>(60)); Save(b,std::vector<char>(60));
    Save(text,std::vector<char>(40)); Save(live,std::vector<char>(40));
    fs::last_write_time(a,old); fs::last_write_time(b,old+std::chrono::minutes(1));
    fs::last_write_time(text,old-std::chrono::minutes(1));
    auto result=StoragePolicy::Enforce(root,140,{live});
    assert(result.before==200 && result.after==140 && result.removed==1);
    assert(!fs::exists(a) && fs::exists(b) && fs::exists(text) && fs::exists(live));
    result=StoragePolicy::Enforce(root,40,{live});
    assert(result.after==40 && !fs::exists(b) && !fs::exists(text) && fs::exists(live));
    Save(a,std::vector<char>(60));
    result=StoragePolicy::Enforce(root,0); assert(fs::exists(a) && result.removed==0);
    fs::last_write_time(live,old);
    result=StoragePolicy::Enforce(root,1,{root/"old",live});
    assert(fs::exists(a) && fs::exists(live) && result.after==100); // protected files can exceed soft cap
    const auto partial=root/"abandoned.dmp.partial";
    Save(partial,std::vector<char>(20));
    assert(StoragePolicy::RemoveIncomplete(root)==1 && !fs::exists(partial));
    // Symlinked unrelated data must never be counted/deleted by retention.
    const auto outside=fs::path(root.string()+"_outside");
    Save(outside/"unrelated.dmp",std::vector<char>(100));
    fs::create_directory_symlink(outside,root/"link");
    result=StoragePolicy::Enforce(root,1,{root/"old",live});
    assert(result.before==100 && fs::exists(outside/"unrelated.dmp"));
    fs::remove_all(root); fs::remove_all(outside);
    std::cout << "Dump validation and storage retention tests passed\n";
}
