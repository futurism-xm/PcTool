#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <unordered_map>
#include <unordered_set>

namespace translation {
// Bind a copy request to a live process object, not just a reusable PID.
// Helpers may own the clipboard, but unrelated processes with the same image
// name, ancestors and sibling applications are not trusted.
class ClipboardSource final {
    struct Handle {
        HANDLE value{};
        ~Handle(){if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    };
public:
    ~ClipboardSource(){Reset();}
    ClipboardSource()=default;
    ClipboardSource(const ClipboardSource&)=delete;
    ClipboardSource& operator=(const ClipboardSource&)=delete;
    void Reset(){if(process_)CloseHandle(process_);process_=nullptr;pid_=0;}
    bool Begin(DWORD pid){
        Reset();process_=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid);
        if(!process_||!ProcessIdToSessionId(pid,&session_)){Reset();return false;}
        pid_=pid;return WaitForSingleObject(process_,0)==WAIT_TIMEOUT;
    }
    bool Accepts(DWORD writer) const {
        if(!writer||!process_||WaitForSingleObject(process_,0)!=WAIT_TIMEOUT)return false;
        if(writer==pid_)return true;
        Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0)};
        if(snapshot.value==INVALID_HANDLE_VALUE)return false;
        std::unordered_map<DWORD,DWORD> parents;
        PROCESSENTRY32W entry{sizeof(entry)};
        if(Process32FirstW(snapshot.value,&entry))do{parents[entry.th32ProcessID]=entry.th32ParentProcessID;}while(Process32NextW(snapshot.value,&entry));
        std::unordered_set<DWORD> visited;
        ULONGLONG childCreated=~ULONGLONG(0);
        for(unsigned depth=0;writer&&depth<32;++depth){
            if(!visited.insert(writer).second)return false;
            Handle current{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,writer)};
            DWORD session{};FILETIME created{},exited{},kernel{},user{};
            if(!current.value||WaitForSingleObject(current.value,0)!=WAIT_TIMEOUT||
                !ProcessIdToSessionId(writer,&session)||session!=session_||
                !GetProcessTimes(current.value,&created,&exited,&kernel,&user))return false;
            const auto stamp=(ULONGLONG(created.dwHighDateTime)<<32)|created.dwLowDateTime;
            if(stamp>childCreated)return false; // recycled parent PID
            if(writer==pid_)return WaitForSingleObject(process_,0)==WAIT_TIMEOUT;
            childCreated=stamp;
            const auto parent=parents.find(writer);if(parent==parents.end())return false;
            writer=parent->second;
        }
        return false;
    }
private:
    HANDLE process_{};DWORD pid_{},session_{};
};
}
