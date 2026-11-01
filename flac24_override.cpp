// Experimental, version-specific x86 runtime setting override. No file patches.
#include <windows.h>
#include <dshow.h>
#include <mmreg.h>
#include <intrin.h>
#include <cstdio>
#include "MinHook.h"

using Setter=HRESULT(STDMETHODCALLTYPE*)(IUnknown*,DWORD);
static Setter original=nullptr;
static HANDLE output=INVALID_HANDLE_VALUE;
static SRWLOCK mutex=SRWLOCK_INIT;
static void record(const char* event,DWORD requested,DWORD applied,HRESULT hr){
    SYSTEMTIME t;GetSystemTime(&t);char line[512];auto n=sprintf_s(line,"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\t%lu\t%lu\t%s\trequested=%lu applied=%lu hr=%08lx\r\n",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,GetCurrentProcessId(),GetCurrentThreadId(),event,requested,applied,ULONG(hr));AcquireSRWLockExclusive(&mutex);DWORD written;if(n>0)WriteFile(output,line,n,&written,nullptr);ReleaseSRWLockExclusive(&mutex);
}
static bool knownCaller(void* returnAddress){HMODULE m=nullptr;if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(returnAddress),&m))return false;wchar_t path[MAX_PATH];GetModuleFileNameW(m,path,MAX_PATH);auto name=wcsrchr(path,L'\\');auto base=reinterpret_cast<ULONG_PTR>(m);auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(m);auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);return _wcsicmp(name?name+1:path,L"OpcFlac.dll")==0&&nt->FileHeader.TimeDateStamp==0x63EB12E0&&reinterpret_cast<ULONG_PTR>(returnAddress)-base==0x32A85;}
static bool isNative96k24(IUnknown* self){
    IBaseFilter* f=nullptr;if(FAILED(self->QueryInterface(IID_PPV_ARGS(&f))))return false;IEnumPins* pins=nullptr;bool found=false;if(SUCCEEDED(f->EnumPins(&pins))){IPin* p=nullptr;while(pins->Next(1,&p,nullptr)==S_OK){PIN_DIRECTION direction;p->QueryDirection(&direction);IEnumMediaTypes* types=nullptr;if(direction==PINDIR_OUTPUT&&SUCCEEDED(p->EnumMediaTypes(&types))){AM_MEDIA_TYPE* mt=nullptr;if(types->Next(1,&mt,nullptr)==S_OK){if(mt->majortype==MEDIATYPE_Audio&&mt->subtype==MEDIASUBTYPE_PCM&&mt->formattype==FORMAT_WaveFormatEx&&mt->cbFormat>=16&&mt->pbFormat){WAVEFORMATEX wave{};memcpy(&wave,mt->pbFormat,min(size_t(mt->cbFormat),sizeof(wave)));found=wave.nSamplesPerSec==96000&&wave.wBitsPerSample==24&&wave.nChannels==2;}if(mt->cbFormat)CoTaskMemFree(mt->pbFormat);if(mt->pUnk)mt->pUnk->Release();CoTaskMemFree(mt);}types->Release();}p->Release();if(found)break;}pins->Release();}f->Release();return found;
}
static HRESULT STDMETHODCALLTYPE setterHook(IUnknown* self,DWORD value){
    bool eligible=value==16&&knownCaller(_ReturnAddress())&&isNative96k24(self);
    if(!eligible){auto hr=original(self,value);record("PASS_THROUGH",value,value,hr);return hr;}
    auto hr=original(self,24);record("SET_24",value,24,hr);
    if(FAILED(hr)){auto fallback=original(self,value);record("SET_16_FALLBACK",value,value,fallback);return fallback;}return hr;
}
static DWORD WINAPI start(void*){
#ifndef _M_IX86
    return 2;
#else
    wchar_t temp[MAX_PATH],path[MAX_PATH];GetTempPathW(MAX_PATH,temp);swprintf_s(path,L"%sFlac24Override-%lu.tsv",temp,GetCurrentProcessId());output=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(output==INVALID_HANDLE_VALUE)return 1;record("START",0,0,S_OK);
    auto co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);CLSID clsid;CLSIDFromString(L"{028A95AA-C956-4874-AD10-C657DB1B00F4}",&clsid);IID iid;CLSIDFromString(L"{1609A1C0-E0E7-4CEF-AA37-31CD15BE4ECD}",&iid);
    HMODULE module=LoadLibraryExW(L"C:\\Program Files (x86)\\Sony\\Music Center\\Sony.Earth\\OmgFlacSrc.ax",nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);IClassFactory* factory=nullptr;IUnknown* source=nullptr;HRESULT hr=E_FAIL;if(module){auto get=reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID,REFIID,void**)>(GetProcAddress(module,"DllGetClassObject"));if(get)hr=get(clsid,IID_PPV_ARGS(&factory));if(factory){hr=factory->CreateInstance(nullptr,iid,reinterpret_cast<void**>(&source));factory->Release();}}
    if(source){auto v=*reinterpret_cast<void***>(source);auto base=reinterpret_cast<ULONG_PTR>(module);auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(module);auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);bool known=nt->FileHeader.TimeDateStamp==0x63EB12C3&&reinterpret_cast<ULONG_PTR>(v[7])-base==0x1860;if(known&&MH_Initialize()==MH_OK){auto status=MH_CreateHook(v[7],reinterpret_cast<void*>(setterHook),reinterpret_cast<void**>(&original));record("CREATE_HOOK",0,0,HRESULT(status));if(status==MH_OK){HMODULE pinned=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(v[7]),&pinned);status=MH_EnableHook(v[7]);}hr=status==MH_OK?S_OK:E_FAIL;}else hr=E_NOINTERFACE;source->Release();}
    if(SUCCEEDED(co))CoUninitialize();record(SUCCEEDED(hr)?"READY":"FAILED",0,0,hr);return SUCCEEDED(hr)?0:2;
#endif
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){DisableThreadLibraryCalls(module);auto t=CreateThread(nullptr,0,start,nullptr,0,nullptr);if(!t)return FALSE;CloseHandle(t);}return TRUE;}
