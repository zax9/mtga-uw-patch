#include "engine.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#else
#include <openssl/evp.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mtga {
namespace {
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
const uint64_t max_image = 512ull * 1024 * 1024;
std::string path_text(const fs::path& p) { return p.u8string(); }
[[noreturn]] void fail(const std::string& s) { throw std::runtime_error(s); }
#ifdef _WIN32
std::string os_error(const std::string& what) { return what + " (Windows error " + std::to_string(GetLastError()) + ")."; }
struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE value) : h(value) {}
    ~Handle() { if (h != INVALID_HANDLE_VALUE && h != nullptr) CloseHandle(h); }
    Handle(const Handle&) = delete; Handle& operator=(const Handle&) = delete;
};
#endif
Bytes read_image(const fs::path& path) {
#ifdef _WIN32
    Handle f(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (f.h == INVALID_HANDLE_VALUE) fail(os_error("Cannot read " + path_text(path)));
    LARGE_INTEGER n;
    if (!GetFileSizeEx(f.h, &n) || n.QuadPart < 1 || uint64_t(n.QuadPart) > max_image) fail("Unexpected image size: " + path_text(path));
    Bytes bytes(static_cast<size_t>(n.QuadPart)); size_t at = 0;
    while (at < bytes.size()) {
        DWORD got = 0, want = static_cast<DWORD>(std::min<size_t>(bytes.size() - at, 1024 * 1024));
        if (!ReadFile(f.h, bytes.data() + at, want, &got, nullptr) || got == 0) fail(os_error("Cannot finish reading " + path_text(path)));
        at += got;
    }
    return bytes;
#else
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) fail("Cannot read " + path_text(path));
    auto n = f.tellg();
    if (n <= 0 || uint64_t(n) > max_image) fail("Unexpected image size: " + path_text(path));
    Bytes bytes(static_cast<size_t>(n)); f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(bytes.data()), n)) fail("Cannot finish reading " + path_text(path));
    return bytes;
#endif
}
std::string sha256(const Bytes& bytes) {
    std::array<uint8_t,32> hash{};
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) fail("Cannot open Windows SHA-256 provider.");
    auto rc = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), hash.data(), 32);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (rc < 0) fail("Windows SHA-256 calculation failed.");
#else
    unsigned length = 0;
    if (EVP_Digest(bytes.data(), bytes.size(), hash.data(), &length, EVP_sha256(), nullptr) != 1 || length != 32) fail("SHA-256 calculation failed.");
#endif
    const char* digits = "0123456789abcdef"; std::string s; s.reserve(64);
    for (auto b : hash) { s.push_back(digits[b >> 4]); s.push_back(digits[b & 15]); }
    return s;
}
void assert_hash(const Bytes& image, const std::string& expected, const char* reason) {
    if (sha256(image) != expected) fail(std::string(reason) + ". Operation stopped.");
}
void remove_quietly(const fs::path& p) { std::error_code e; fs::remove(p,e); }
bool reparse(const fs::path& p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) fail(os_error("Cannot inspect " + path_text(p)));
    return (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return fs::is_symlink(fs::symlink_status(p));
#endif
}
void write_new(const fs::path& path, const Bytes& bytes) {
    bool created = false;
    try {
#ifdef _WIN32
        {
            Handle f(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (f.h == INVALID_HANDLE_VALUE) fail(os_error("Cannot create " + path_text(path)));
            created = true; size_t at = 0;
            while (at < bytes.size()) {
                DWORD done = 0, want = static_cast<DWORD>(std::min<size_t>(bytes.size()-at,1024*1024));
                if (!WriteFile(f.h, bytes.data()+at,want,&done,nullptr) || done == 0) fail(os_error("Cannot write " + path_text(path)));
                at += done;
            }
            if (!FlushFileBuffers(f.h)) fail(os_error("Cannot flush " + path_text(path)));
        }
#else
        int fd = ::open(path.c_str(), O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
        if(fd < 0) fail("Cannot create " + path_text(path));
        created = true;
        try {
            size_t at=0;
            while(at<bytes.size()) { auto n=::write(fd,bytes.data()+at,bytes.size()-at); if(n<0 && errno==EINTR)continue; if(n<=0)fail("Cannot write staging file."); at+=static_cast<size_t>(n); }
            if(::fsync(fd)!=0)fail("Cannot flush staging file.");
        } catch(...) { ::close(fd); throw; }
        ::close(fd);
#endif
    } catch (...) { if(created) remove_quietly(path); throw; }
}
struct OperationLock {
    fs::path path; bool owns = false;
#ifdef _WIN32
    HANDLE h = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
    explicit OperationLock(fs::path p): path(std::move(p)) {
#ifdef _WIN32
        h=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        owns=h!=INVALID_HANDLE_VALUE;
#else
        fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600); owns=fd>=0;
#endif
        if(!owns)fail("Cannot create patch lock. Another patcher may be running, a stale lock may remain, or the folder is not writable. Close all patcher windows before removing a stale .Core.dll.ultrawide-patch.lock.");
    }
    ~OperationLock() { if(owns) {
#ifdef _WIN32
        CloseHandle(h);
#else
        ::close(fd);
#endif
        remove_quietly(path);
    } }
};
void ensure_closed() {
#ifdef _WIN32
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));
    if(snapshot.h==INVALID_HANDLE_VALUE)fail(os_error("Cannot inspect running processes"));
    PROCESSENTRY32W p{};p.dwSize=sizeof(p);
    if(!Process32FirstW(snapshot.h,&p))fail(os_error("Cannot enumerate running processes"));
    do {
        if(_wcsicmp(p.szExeFile,L"MTGA.exe")==0 || _wcsicmp(p.szExeFile,L"MTGALauncher.exe")==0 || _wcsicmp(p.szExeFile,L"MTGAUpdater.exe")==0)
            fail("Close Arena and its launcher/updater before installing or restoring.");
    } while(Process32NextW(snapshot.h,&p));
    if(GetLastError()!=ERROR_NO_MORE_FILES)fail(os_error("Process enumeration failed"));
#else
    for(const auto& entry:fs::directory_iterator("/proc")) {
        auto name=entry.path().filename().string();
        if(name.empty() || !std::all_of(name.begin(),name.end(),[](char c){return c>='0'&&c<='9';}))continue;
        std::ifstream f(entry.path()/"comm");std::string s;std::getline(f,s);
        if(s=="MTGA"||s=="MTGALauncher"||s=="MTGAUpdater")fail("Close Arena and its launcher/updater before installing or restoring.");
    }
#endif
}
void ensure_permission(const fs::path& target) {
#ifdef _WIN32
    bool protected_folder=false;
    for(const wchar_t* name:{L"ProgramFiles",L"ProgramFiles(x86)",L"ProgramW6432"}) {
        wchar_t value[32768];DWORD n=GetEnvironmentVariableW(name,value,32768);
        if(n==0||n>=32768)continue;
        std::wstring prefix=fs::absolute(fs::path(value)).lexically_normal().wstring();
        while(!prefix.empty()&&(prefix.back()==L'\\'||prefix.back()==L'/'))prefix.pop_back();prefix+=L'\\';
        auto t=target.wstring();
        if(t.size()>=prefix.size()&&_wcsnicmp(t.c_str(),prefix.c_str(),prefix.size())==0)protected_folder=true;
    }
    if(!protected_folder)return;
    SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY;PSID sid=nullptr;BOOL member=FALSE;
    if(!AllocateAndInitializeSid(&authority,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&sid))fail(os_error("Cannot inspect administrator access"));
    BOOL ok=CheckTokenMembership(nullptr,sid,&member);FreeSid(sid);
    if(!ok)fail(os_error("Cannot inspect administrator access"));
    if(!member)fail("Writing Program Files requires administrator access. Close this window, right-click MTGAUltrawidePatcher.exe, choose Run as administrator, then click Install or Restore.");
#else
    (void)target;
#endif
}
Bytes patched(const Bytes& original,const MatchResult& m) {
    if(m.status!=MatchStatus::Patchable || m.upper_offset>original.size() || original.size()-m.upper_offset<4)fail("No verified patch operand.");
    Bytes result=original;uint32_t bits=0;float limit=2.4f;std::memcpy(&bits,&limit,4);
    for(int i=0;i<4;i++)result[m.upper_offset+i]=static_cast<uint8_t>(bits>>(8*i));
    auto check=inspect(result);
    if(check.status!=MatchStatus::AlreadyCompatible||check.upper_offset!=m.upper_offset||check.original_upper!=limit||check.lower!=m.lower)fail("Patched method failed semantic verification.");
    return result;
}
struct Backup { fs::path path; Bytes bytes; };
std::optional<Backup> matching_backup(const fs::path& target,const Bytes& current) {
    const std::string prefix="Core.dll.ultrawide.",suffix=".original.bak";
    std::string target_hash=sha256(current);std::optional<Backup> found;size_t count=0;
    for(const auto& e:fs::directory_iterator(target.parent_path())) {
        auto name=e.path().filename().u8string();
        if(name.size()!=prefix.size()+64+suffix.size()||name.compare(0,prefix.size(),prefix)!=0||name.compare(name.size()-suffix.size(),suffix.size(),suffix)!=0)continue;
        if(++count>128)fail("Too many original backups. Archive old backups before restoring.");
        auto hash=name.substr(prefix.size(),64);
        if(!std::all_of(hash.begin(),hash.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');}))continue;
        if(reparse(e.path())||!fs::is_regular_file(e.path())||fs::file_size(e.path())!=current.size())continue;
        auto bytes=read_image(e.path());if(sha256(bytes)!=hash)continue;
        auto m=inspect(bytes);if(m.status!=MatchStatus::Patchable)continue;
        if(sha256(patched(bytes,m))!=target_hash)continue;
        if(found)fail("More than one original backup can produce this patched file. Restore is ambiguous and has been refused.");
        found=Backup{e.path(),std::move(bytes)};
    }
    return found;
}
std::string unique_id() {
    std::array<uint8_t,16> data{};
#ifdef _WIN32
    if(BCryptGenRandom(nullptr,data.data(),static_cast<ULONG>(data.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)fail("Cannot create a unique staging name.");
#else
    std::ifstream f("/dev/urandom",std::ios::binary);if(!f.read(reinterpret_cast<char*>(data.data()),data.size()))fail("Cannot create a unique staging name.");
#endif
    const char*digits="0123456789abcdef";std::string s;for(auto b:data){s+=digits[b>>4];s+=digits[b&15];}return s;
}
void replace_file(const fs::path& target,const fs::path& stage,const fs::path& rollback,const Bytes& previous) {
#ifdef _WIN32
    (void)previous;
    if(!ReplaceFileW(target.c_str(),stage.c_str(),rollback.c_str(),0,nullptr,nullptr))fail(os_error("Windows could not replace Core.dll"));
#else
    write_new(rollback,previous);fs::rename(stage,target);
#endif
}
Report action_impl(Action action,const fs::path& game) {
    Report report;auto root=fs::absolute(game).lexically_normal();
    if(!fs::is_directory(root))fail("Arena installation folder not found: "+path_text(root));
    auto target=root/"MTGA_Data"/"Managed"/"Core.dll";
    if(!fs::is_regular_file(target))fail("Core.dll was not found. Select the folder containing MTGA.exe and MTGA_Data.");
    if(reparse(target))fail("Core.dll must be a regular file, not a symbolic link.");
    auto current=read_image(target);auto hash=sha256(current);auto m=inspect(current);report.status=m.status;
    std::ostringstream detail;detail<<"Target: "<<path_text(target)<<"\r\nSHA256: "<<hash<<"\r\n"<<m.reason;
    report.detail=detail.str();
    if(m.status==MatchStatus::Unsupported){report.headline="Unsupported build - no changes made";return report;}
    report.success=true;report.can_install=m.status==MatchStatus::Patchable;
    if(action==Action::Status) {
        report.headline=report.can_install?"Ready to patch":"Already compatible";
        if(m.status==MatchStatus::AlreadyCompatible){auto b=matching_backup(target,current);report.can_restore=b.has_value();report.detail+=b?"\r\nVerified original backup: "+path_text(b->path):"\r\nNo matching original backup was found.";}
        return report;
    }
    if(action==Action::Install&&m.status==MatchStatus::AlreadyCompatible){report.headline="Already compatible - no changes needed";auto b=matching_backup(target,current);report.can_restore=b.has_value();return report;}
    if(action==Action::Restore&&m.status==MatchStatus::Patchable){report.headline="Original aspect limit is present - nothing to restore";return report;}
    ensure_closed();ensure_permission(target);
    OperationLock lock(target.parent_path()/".Core.dll.ultrawide-patch.lock");
    assert_hash(read_image(target),hash,"Core.dll changed before acquiring the lock");
    Bytes next;fs::path backup;
    if(action==Action::Install) {
        next=patched(current,m);backup=target.parent_path()/("Core.dll.ultrawide."+hash+".original.bak");
        if(!fs::exists(backup))write_new(backup,current);
        if(reparse(backup))fail("Original backup must not be a symbolic link.");
        assert_hash(read_image(backup),hash,"Original backup does not match this build");
    } else {
        auto b=matching_backup(target,current);if(!b)fail("No verified original backup matches this exact patched build. Restore refused; an older build will not be copied over it.");
        backup=b->path;next=std::move(b->bytes);
    }
    auto next_hash=sha256(next);auto id=unique_id();
    auto stage=target.parent_path()/(".Core.dll.ultrawide."+id+".staged");
    auto rollback=target.parent_path()/("Core.dll.ultrawide."+id+".before-replace.bak");
    bool replacement_attempted=false;
    try {
        write_new(stage,next);assert_hash(read_image(stage),next_hash,"Staged image verification failed");ensure_closed();
        assert_hash(read_image(target),hash,"Core.dll changed immediately before replacement");
        if(fs::exists(rollback))fail("Recovery filename collision.");
        replacement_attempted=true;replace_file(target,stage,rollback,current);
        assert_hash(read_image(target),next_hash,"Installed image verification failed");
        assert_hash(read_image(rollback),hash,"The replaced file differed from the verified input");
        fs::remove(rollback);
    } catch(const std::exception& e) {
        if(!replacement_attempted)remove_quietly(stage);
        std::string message=e.what();
        if(replacement_attempted)message+="\r\nReplacement was attempted. Do not launch Arena until its file state is checked. Original backup: "+path_text(backup)+"\r\nRetained recovery/staging paths (if present): "+path_text(rollback)+" ; "+path_text(stage);
        fail(message);
    }
    report.status=action==Action::Install?MatchStatus::AlreadyCompatible:MatchStatus::Patchable;
    report.can_install=action==Action::Restore;report.can_restore=action==Action::Install;
    report.headline=action==Action::Install?"Patch installed and verified":"Original file restored and verified";
    report.detail="Result SHA256: "+next_hash+"\r\nOriginal backup retained: "+path_text(backup)+"\r\nStart Arena normally. No keyboard input is sent to the game.";
    return report;
}
}
Report execute(Action action,const std::filesystem::path& game_path) {
    try{return action_impl(action,game_path);}
    catch(const std::exception& e){Report r;r.headline="Operation stopped";r.detail=e.what();return r;}
}
}
