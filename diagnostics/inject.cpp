#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cwchar>
static int fail(const wchar_t* message) {fwprintf(stderr,L"%s (error=%lu)\n",message,GetLastError());return 1;}
int wmain(int argc,wchar_t** argv) {
    if(argc!=3) {fwprintf(stderr,L"Usage: inject.exe PID absolute-path-to-probe.dll\n");return 2;}
    wchar_t* end=nullptr;unsigned long pid=wcstoul(argv[1],&end,10);if(!pid||*end)return 2;
    wchar_t path[32768];if(!GetFullPathNameW(argv[2],32768,path,nullptr)||GetFileAttributesW(path)==INVALID_FILE_ATTRIBUTES)return fail(L"DLL not found");
    HANDLE process=OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ,FALSE,pid);if(!process)return fail(L"OpenProcess");
    USHORT target=0,native=0,self=0,selfNative=0;
    if(!IsWow64Process2(process,&target,&native)||!IsWow64Process2(GetCurrentProcess(),&self,&selfNative))return fail(L"IsWow64Process2");
    if(target!=self||native!=selfNative) {fwprintf(stderr,L"Architecture mismatch. Use the matching x86/x64 injector and DLL.\n");CloseHandle(process);return 2;}
    // Resolve the actual owner of LoadLibraryW (it may be forwarded to KernelBase).
    auto load=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");HMODULE owner=nullptr;wchar_t ownerPath[MAX_PATH];
    if(!load||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(load),&owner))return fail(L"Resolve LoadLibraryW");
    GetModuleFileNameW(owner,ownerPath,MAX_PATH);const wchar_t* name=wcsrchr(ownerPath,L'\\');name=name?name+1:ownerPath;
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);if(snapshot==INVALID_HANDLE_VALUE)return fail(L"Module snapshot");
    MODULEENTRY32W entry{};entry.dwSize=sizeof(entry);BYTE* remoteBase=nullptr;
    if(Module32FirstW(snapshot,&entry))do {if(!_wcsicmp(entry.szModule,name)){remoteBase=entry.modBaseAddr;break;}}while(Module32NextW(snapshot,&entry));CloseHandle(snapshot);
    if(!remoteBase)return fail(L"Remote loader module not found");
    SIZE_T size=(wcslen(path)+1)*sizeof(wchar_t);void* remote=VirtualAllocEx(process,nullptr,size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);if(!remote)return fail(L"VirtualAllocEx");
    if(!WriteProcessMemory(process,remote,path,size,nullptr))return fail(L"WriteProcessMemory");
    auto start=reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteBase+(reinterpret_cast<BYTE*>(load)-reinterpret_cast<BYTE*>(owner)));
    HANDLE thread=CreateRemoteThread(process,nullptr,0,start,remote,0,nullptr);if(!thread)return fail(L"CreateRemoteThread");
    if(WaitForSingleObject(thread,15000)!=WAIT_OBJECT_0) {fwprintf(stderr,L"Loader timeout; remote argument retained. Check process/log before retry.\n");return 1;}
    DWORD code=0;GetExitCodeThread(thread,&code);VirtualFreeEx(process,remote,0,MEM_RELEASE);CloseHandle(thread);CloseHandle(process);
    // x64 HMODULE is truncated by thread exit code; verify via module snapshot instead.
    snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);bool found=false;entry.dwSize=sizeof(entry);
    if(snapshot!=INVALID_HANDLE_VALUE){if(Module32FirstW(snapshot,&entry))do {if(!_wcsicmp(entry.szExePath,path)){found=true;break;}}while(Module32NextW(snapshot,&entry));CloseHandle(snapshot);}
    if(!found){fwprintf(stderr,L"DLL load not verified (loader result low DWORD=%lu).\n",code);return 1;}
    wprintf(L"DLL loaded. Wait for READY in %%TEMP%%\\AudioFormatProbe-%lu.tsv, then recreate playback. Injection alone does not mean hooks are ready.\n",pid);return 0;
}
