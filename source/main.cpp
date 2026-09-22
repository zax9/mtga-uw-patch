#include "engine.h"
#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
int run_gui(HINSTANCE,int);
#endif
namespace {
const char* state(mtga::MatchStatus s) {
    switch(s){case mtga::MatchStatus::Patchable:return "Patchable";case mtga::MatchStatus::AlreadyCompatible:return "AlreadyCompatible";default:return "Unsupported";}
}
std::string help_text() {
    return "MTGA Ultrawide Patcher 3.0.0 (native)\n"
        "MTGAUltrawidePatcher.exe [status|install|restore] [--game-path \"Arena folder\"]\n"
        "No arguments opens the Windows GUI. Close Arena before Install/Restore.\n"
        "Install/Restore in Program Files require administrator access.\n";
}
int cli(const std::vector<std::filesystem::path>& args,std::string& output) {
    try {
        mtga::Action action=mtga::Action::Status;bool seen=false;
        std::filesystem::path folder=L"C:\\Program Files\\Wizards of the Coast\\MTGA";
        for(size_t i=0;i<args.size();i++) {
            auto arg=args[i].u8string();
            if(arg=="--help"||arg=="-h"||arg=="/?"){output=help_text();return 0;}
            if(arg=="--game-path") {if(++i==args.size())throw std::runtime_error("--game-path requires a folder.");folder=args[i];}
            else if(arg=="status"||arg=="install"||arg=="restore") {
                if(seen) throw std::runtime_error("Specify only one action.");
                seen=true;
                action=arg=="status"?mtga::Action::Status:arg=="install"?mtga::Action::Install:mtga::Action::Restore;
            } else throw std::runtime_error("Unknown argument: "+arg);
        }
        auto r=mtga::execute(action,folder);
        output=std::string("MTGA Ultrawide Patcher 3.0.0 (native)\nStatus: ")+state(r.status)+"\n"+r.headline+"\n"+r.detail+"\n";
        return r.success?0:1;
    }catch(const std::exception& e){output=std::string("ERROR: ")+e.what()+"\n";return 1;}
}
}
#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(!argv)return 1;
    if(argc<=1){LocalFree(argv);return run_gui(instance,show);}
    std::vector<std::filesystem::path> args;for(int i=1;i<argc;i++)args.emplace_back(argv[i]);LocalFree(argv);
    HANDLE out=GetStdHandle(STD_OUTPUT_HANDLE);
    if(out==nullptr||out==INVALID_HANDLE_VALUE){AttachConsole(ATTACH_PARENT_PROCESS);out=GetStdHandle(STD_OUTPUT_HANDLE);}
    std::string text;int rc=cli(args,text);
    if(out!=nullptr&&out!=INVALID_HANDLE_VALUE){DWORD done=0;WriteFile(out,text.data(),static_cast<DWORD>(text.size()),&done,nullptr);}
    return rc;
}
#else
int main(int argc,char**argv) {
    std::vector<std::filesystem::path> args;for(int i=1;i<argc;i++)args.emplace_back(argv[i]);
    if(args.empty()){std::cout<<help_text();return 0;}
    std::string text;int rc=cli(args,text);std::cout<<text;return rc;
}
#endif
