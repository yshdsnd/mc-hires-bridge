// Experimental, version-specific x86 runtime setting override. No file patches.
#include <windows.h>
#include <dshow.h>
#include <mmreg.h>
#include <intrin.h>
#include <cstdio>
#include <map>
#include "MinHook.h"
#include "bridge_status.h"
#include "bridge_startup.h"
#ifdef EXPERIMENT_WAV192
#include "wav192_route.h"
#include "wav192_config.h"
#endif

using Setter=HRESULT(STDMETHODCALLTYPE*)(IUnknown*,DWORD);
static Setter original=nullptr;
static bool supportedFlacRate(DWORD rate){return rate==48000||rate==96000||rate==192000;}
static HANDLE output=INVALID_HANDLE_VALUE;
static SRWLOCK mutex=SRWLOCK_INIT;
static HINSTANCE ownModule=nullptr;
static SRWLOCK sourcesLock=SRWLOCK_INIT;
static std::map<IBaseFilter*,bool> appliedSources;
static void rememberSource(IUnknown* self,bool applied){IBaseFilter* f=nullptr;if(SUCCEEDED(self->QueryInterface(IID_PPV_ARGS(&f)))){AcquireSRWLockExclusive(&sourcesLock);if(appliedSources.size()>=64&&appliedSources.find(f)==appliedSources.end())appliedSources.erase(appliedSources.begin());appliedSources[f]=applied;ReleaseSRWLockExclusive(&sourcesLock);f->Release();}}
static bool sourceApplied(IBaseFilter* f){CLSID cls{},flac;CLSIDFromString(L"{028A95AA-C956-4874-AD10-C657DB1B00F4}",&flac);if(FAILED(f->GetClassID(&cls)))return false;
#ifdef EXPERIMENT_WAV192
    if(cls!=flac&&!wav192::isParser(cls))return false;
#else
    if(cls!=flac)return false;
#endif
    AcquireSRWLockShared(&sourcesLock);auto it=appliedSources.find(f);bool applied=it!=appliedSources.end()&&it->second;ReleaseSRWLockShared(&sourcesLock);return applied;}
static void record(const char* event,DWORD requested,DWORD applied,HRESULT hr){
    SYSTEMTIME t;GetSystemTime(&t);char line[512];auto n=sprintf_s(line,"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\t%lu\t%lu\t%s\trequested=%lu applied=%lu hr=%08lx\r\n",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,GetCurrentProcessId(),GetCurrentThreadId(),event,requested,applied,ULONG(hr));AcquireSRWLockExclusive(&mutex);DWORD written;if(n>0)WriteFile(output,line,n,&written,nullptr);ReleaseSRWLockExclusive(&mutex);
}
// OpcFlac uses distinct calls for initial playback and seamless next-track preparation.
static bool knownCaller(void* returnAddress){HMODULE m=nullptr;if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(returnAddress),&m))return false;wchar_t path[MAX_PATH];GetModuleFileNameW(m,path,MAX_PATH);auto name=wcsrchr(path,L'\\');auto base=reinterpret_cast<ULONG_PTR>(m);auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(m);auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);return _wcsicmp(name?name+1:path,L"OpcFlac.dll")==0&&nt->FileHeader.TimeDateStamp==0x63EB12E0&&(reinterpret_cast<ULONG_PTR>(returnAddress)-base==0x32A85||reinterpret_cast<ULONG_PTR>(returnAddress)-base==0x31E7A);}
static bool isNativeSupported24(IUnknown* self){
    IBaseFilter* f=nullptr;if(FAILED(self->QueryInterface(IID_PPV_ARGS(&f))))return false;IEnumPins* pins=nullptr;bool found=false;if(SUCCEEDED(f->EnumPins(&pins))){IPin* p=nullptr;while(pins->Next(1,&p,nullptr)==S_OK){PIN_DIRECTION direction;p->QueryDirection(&direction);IEnumMediaTypes* types=nullptr;if(direction==PINDIR_OUTPUT&&SUCCEEDED(p->EnumMediaTypes(&types))){AM_MEDIA_TYPE* mt=nullptr;if(types->Next(1,&mt,nullptr)==S_OK){if(mt->majortype==MEDIATYPE_Audio&&mt->subtype==MEDIASUBTYPE_PCM&&mt->formattype==FORMAT_WaveFormatEx&&mt->cbFormat>=16&&mt->pbFormat){WAVEFORMATEX wave{};memcpy(&wave,mt->pbFormat,min(size_t(mt->cbFormat),sizeof(wave)));found=supportedFlacRate(wave.nSamplesPerSec)&&wave.wBitsPerSample==24&&wave.nChannels==2;}if(mt->cbFormat)CoTaskMemFree(mt->pbFormat);if(mt->pUnk)mt->pUnk->Release();CoTaskMemFree(mt);}types->Release();}p->Release();if(found)break;}pins->Release();}f->Release();return found;
}
static HRESULT STDMETHODCALLTYPE setterHook(IUnknown* self,DWORD value){
    bool eligible=value==16&&knownCaller(_ReturnAddress())&&bridgeStatus::directSoundSelected()&&isNativeSupported24(self);
    if(!eligible){auto hr=original(self,value);rememberSource(self,false);record("PASS_THROUGH",value,value,hr);return hr;}
    auto hr=original(self,24);record("SET_24",value,24,hr);
    rememberSource(self,SUCCEEDED(hr));
    if(FAILED(hr)){auto fallback=original(self,value);record("SET_16_FALLBACK",value,value,fallback);return fallback;}return hr;
}
using Run=HRESULT(STDMETHODCALLTYPE*)(IMediaControl*);
using FilterRun=HRESULT(STDMETHODCALLTYPE*)(IMediaFilter*,REFERENCE_TIME);
static Run originalRun=nullptr;static FilterRun originalFilterRun=nullptr;
static bool supportedOutputRate(DWORD rate){return supportedFlacRate(rate);}
#ifdef EXPERIMENT_WAV192
static thread_local bool preparingWav=false;
using DseeSetter=HRESULT(STDMETHODCALLTYPE*)(IUnknown*,wav192::DseeConfig);
static DseeSetter originalDsee=nullptr;
static Run originalPause=nullptr;
static HRESULT(STDMETHODCALLTYPE* originalFilterPause)(IMediaFilter*)=nullptr;
static bool knownWavCaller(void* returnAddress){
    HMODULE m=nullptr;if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(returnAddress),&m))return false;
    wchar_t path[MAX_PATH]{};GetModuleFileNameW(m,path,MAX_PATH);auto name=wcsrchr(path,L'\\');auto base=reinterpret_cast<BYTE*>(m);auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(m);auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    return _wcsicmp(name?name+1:path,L"OpcWavEx.dll")==0&&nt->FileHeader.TimeDateStamp==0x63eb12e7&&reinterpret_cast<BYTE*>(returnAddress)-base==0x33e47;
}
static HRESULT STDMETHODCALLTYPE dseeSetterHook(IUnknown* self,wav192::DseeConfig config){
    record("DSEE_REQUEST_INPUT",config.inputRate,config.inputBits,knownWavCaller(_ReturnAddress())?S_OK:S_FALSE);
    record("DSEE_REQUEST_OUTPUT",config.outputRate,config.outputBits,S_OK);
    record("DSEE_REQUEST_MODE_FORMAT",config.mode,config.format,HRESULT(config.channels));
    wav192::DseeConfig adjusted{};
    if(!knownWavCaller(_ReturnAddress())||!bridgeStatus::directSoundSelected()||!wav192::nativeConfig(config,adjusted)){
        bool previouslyChanged=false;Microsoft::WRL::ComPtr<IBaseFilter> filter;
        if(SUCCEEDED(self->QueryInterface(IID_PPV_ARGS(&filter)))){AcquireSRWLockShared(&sourcesLock);auto it=appliedSources.find(filter.Get());previouslyChanged=it!=appliedSources.end()&&it->second;ReleaseSRWLockShared(&sourcesLock);}
        auto hr=originalDsee(self,config);
        bool keepsNative=config.channels==2&&config.inputRate==192000&&config.outputRate==192000&&config.inputBits==24&&config.outputBits==24&&config.format==24;
        rememberSource(self,previouslyChanged&&keepsNative&&SUCCEEDED(hr));return hr;
    }
    auto hr=originalDsee(self,adjusted);rememberSource(self,SUCCEEDED(hr));record("WAV192_DSEE_NATIVE",config.outputRate,adjusted.outputRate,hr);
    if(FAILED(hr)){auto fallback=originalDsee(self,config);record("WAV192_DSEE_FALLBACK",config.outputRate,config.outputRate,fallback);return fallback;}
    return hr;
}
static bool setupDseeHook(){
    auto module=LoadLibraryExW(L"C:\\Program Files (x86)\\Sony\\Music Center\\Sony.Earth\\OmgDseeHxFilter.ax",nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if(!module)return false;auto get=reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID,REFIID,void**)>(GetProcAddress(module,"DllGetClassObject"));if(!get)return false;
    Microsoft::WRL::ComPtr<IClassFactory> factory;Microsoft::WRL::ComPtr<IUnknown> control;IID iid;CLSIDFromString(L"{6AF76DBE-8634-4FD3-93ED-982EDE1BFDD5}",&iid);
    if(FAILED(get(wav192::dseeId,IID_PPV_ARGS(&factory)))||FAILED(factory->CreateInstance(nullptr,iid,reinterpret_cast<void**>(control.GetAddressOf()))))return false;
    auto base=reinterpret_cast<BYTE*>(module);auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);auto fn=(*reinterpret_cast<void***>(control.Get()))[3];
    if(nt->FileHeader.TimeDateStamp!=0x63eb12c1||reinterpret_cast<BYTE*>(fn)-base!=0x36ca)return false;
    auto status=MH_CreateHook(fn,reinterpret_cast<void*>(dseeSetterHook),reinterpret_cast<void**>(&originalDsee));record("CREATE_DSEE_HOOK",0,0,HRESULT(status));
    if(status==MH_OK){HMODULE pinned=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(fn),&pinned);}return status==MH_OK;
}
static bool configuredDseeStillConnected(IGraphBuilder* graph){
    Microsoft::WRL::ComPtr<IEnumFilters> filters;if(FAILED(graph->EnumFilters(&filters)))return true;
    Microsoft::WRL::ComPtr<IBaseFilter> f;while(filters->Next(1,&f,nullptr)==S_OK){CLSID cls{};
        if(SUCCEEDED(f->GetClassID(&cls))&&cls==wav192::dseeId){AcquireSRWLockShared(&sourcesLock);auto it=appliedSources.find(f.Get());bool changed=it!=appliedSources.end()&&it->second;ReleaseSRWLockShared(&sourcesLock);
            if(changed){for(auto direction:{PINDIR_INPUT,PINDIR_OUTPUT}){auto pin=wav192::uniquePin(f.Get(),direction);Microsoft::WRL::ComPtr<IPin> peer;if(!pin||SUCCEEDED(pin->ConnectedTo(&peer)))return true;}}}
        f.Reset();}return false;
}
static HRESULT prepareWav(IUnknown* object){
    if(!bridgeStatus::directSoundSelected())return S_OK;
    Microsoft::WRL::ComPtr<IGraphBuilder> graph;
    if(FAILED(object->QueryInterface(IID_PPV_ARGS(&graph))))return S_OK;
    Microsoft::WRL::ComPtr<IEnumFilters> diagnostic;
    if(SUCCEEDED(graph->EnumFilters(&diagnostic))){Microsoft::WRL::ComPtr<IBaseFilter> filter;while(diagnostic->Next(1,&filter,nullptr)==S_OK){CLSID cls{};filter->GetClassID(&cls);wchar_t id[40];StringFromGUID2(cls,id,40);char line[512];auto n=sprintf_s(line,"GRAPH_FILTER\tcls=%ls\r\n",id);AcquireSRWLockExclusive(&mutex);DWORD written;if(n>0)WriteFile(output,line,n,&written,nullptr);ReleaseSRWLockExclusive(&mutex);Microsoft::WRL::ComPtr<IEnumPins> pins;if(SUCCEEDED(filter->EnumPins(&pins))){Microsoft::WRL::ComPtr<IPin> pin;while(pins->Next(1,&pin,nullptr)==S_OK){AM_MEDIA_TYPE mt{};PIN_DIRECTION d{};pin->QueryDirection(&d);if(SUCCEEDED(pin->ConnectionMediaType(&mt))&&mt.formattype==FORMAT_WaveFormatEx&&mt.pbFormat&&mt.cbFormat>=16){WAVEFORMATEX wave{};memcpy(&wave,mt.pbFormat,min(size_t(mt.cbFormat),sizeof(wave)));record(d==PINDIR_INPUT?"GRAPH_INPUT":"GRAPH_OUTPUT",wave.nSamplesPerSec,wave.wBitsPerSample,HRESULT(wave.nChannels));}if(mt.cbFormat)CoTaskMemFree(mt.pbFormat);if(mt.pUnk)mt.pUnk->Release();pin.Reset();}}filter.Reset();}}
    auto result=wav192::apply(graph.Get());
    const char* event=result.outcome==wav192::Outcome::Applied?"WAV192_APPLIED":result.outcome==wav192::Outcome::AlreadyNative?"WAV192_NATIVE":result.outcome==wav192::Outcome::Restored?"WAV192_RESTORED":result.outcome==wav192::Outcome::RestoreFailed?"WAV192_RESTORE_FAILED":"WAV192_SKIPPED";
    record(event,DWORD(result.outcome),DWORD(result.restore),result.change);
    // A native-configured DSEE must be bypassed; its independent tail test lost samples.
    if(configuredDseeStillConnected(graph.Get())){record("WAV192_BYPASS_REQUIRED",0,0,E_FAIL);bridgeStatus::state.store(bridgeStatus::State::Off);return E_FAIL;}
    if(result.outcome==wav192::Outcome::Applied||result.outcome==wav192::Outcome::AlreadyNative){
        Microsoft::WRL::ComPtr<IEnumFilters> filters;
        if(SUCCEEDED(graph->EnumFilters(&filters))){Microsoft::WRL::ComPtr<IBaseFilter> f;while(filters->Next(1,&f,nullptr)==S_OK){CLSID cls{};if(SUCCEEDED(f->GetClassID(&cls))&&wav192::isParser(cls))rememberSource(f.Get(),true);f.Reset();}}
    }
    if(result.outcome==wav192::Outcome::RestoreFailed){bridgeStatus::state.store(bridgeStatus::State::Off);return E_FAIL;}
    return S_OK;
}
struct WavScope {bool outer;WavScope():outer(!preparingWav){preparingWav=true;}~WavScope(){if(outer)preparingWav=false;}};
static HRESULT STDMETHODCALLTYPE pauseHook(IMediaControl* self){WavScope scope;if(scope.outer){auto hr=prepareWav(self);if(FAILED(hr))return hr;}return originalPause(self);}
static HRESULT STDMETHODCALLTYPE filterPauseHook(IMediaFilter* self){WavScope scope;if(scope.outer){auto hr=prepareWav(self);if(FAILED(hr))return hr;}return originalFilterPause(self);}
#endif
static void updateStatus(IUnknown* object,HRESULT result) {
    bool source=false,output24=false;IFilterGraph* graph=nullptr;
    if(SUCCEEDED(result)&&bridgeStatus::directSoundSelected()&&SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&graph)))){
        IEnumFilters* filters=nullptr;if(SUCCEEDED(graph->EnumFilters(&filters))){IBaseFilter* f=nullptr;while(filters->Next(1,&f,nullptr)==S_OK){CLSID cls{};f->GetClassID(&cls);if(sourceApplied(f))source=true;
            if(cls==CLSID_DSoundRender){IEnumPins* pins=nullptr;if(SUCCEEDED(f->EnumPins(&pins))){IPin* p=nullptr;while(pins->Next(1,&p,nullptr)==S_OK){AM_MEDIA_TYPE mt{};if(SUCCEEDED(p->ConnectionMediaType(&mt))&&mt.formattype==FORMAT_WaveFormatEx&&mt.cbFormat>=16&&mt.pbFormat){WAVEFORMATEX wave{};memcpy(&wave,mt.pbFormat,min(size_t(mt.cbFormat),sizeof(wave)));if(supportedOutputRate(wave.nSamplesPerSec)&&wave.wBitsPerSample==24&&wave.nChannels==2)output24=true;}if(mt.cbFormat)CoTaskMemFree(mt.pbFormat);if(mt.pUnk)mt.pUnk->Release();p->Release();}pins->Release();}}
            f->Release();}filters->Release();}graph->Release();}
    auto next=source&&output24?bridgeStatus::State::On:bridgeStatus::State::Off;
    bridgeStatus::state.store(next);record(next==bridgeStatus::State::On?"STATUS_ON":"STATUS_OFF",0,0,result);
}
static HRESULT STDMETHODCALLTYPE runHook(IMediaControl* self){
#ifdef EXPERIMENT_WAV192
    WavScope scope;if(scope.outer){auto check=prepareWav(self);if(FAILED(check))return check;}
#endif
    auto hr=originalRun(self);updateStatus(self,hr);return hr;}
static HRESULT STDMETHODCALLTYPE filterRunHook(IMediaFilter* self,REFERENCE_TIME time){
#ifdef EXPERIMENT_WAV192
    WavScope scope;if(scope.outer){auto check=prepareWav(self);if(FAILED(check))return check;}
#endif
    auto hr=originalFilterRun(self,time);updateStatus(self,hr);return hr;}
static bool setupRunHooks(){IGraphBuilder* graph=nullptr;if(FAILED(CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&graph))))return false;bool ok=false;IMediaControl* c=nullptr;IMediaFilter* f=nullptr;if(SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&c)))&&SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&f)))){auto a=(*reinterpret_cast<void***>(c))[7],b=(*reinterpret_cast<void***>(f))[6];HMODULE pinned=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(a),&pinned);ok=MH_CreateHook(a,reinterpret_cast<void*>(runHook),reinterpret_cast<void**>(&originalRun))==MH_OK&&MH_CreateHook(b,reinterpret_cast<void*>(filterRunHook),reinterpret_cast<void**>(&originalFilterRun))==MH_OK;}
#ifdef EXPERIMENT_WAV192
if(ok){auto pause=(*reinterpret_cast<void***>(c))[8],filterPause=(*reinterpret_cast<void***>(f))[5];ok=MH_CreateHook(pause,reinterpret_cast<void*>(pauseHook),reinterpret_cast<void**>(&originalPause))==MH_OK&&MH_CreateHook(filterPause,reinterpret_cast<void*>(filterPauseHook),reinterpret_cast<void**>(&originalFilterPause))==MH_OK;}
#endif
if(c)c->Release();if(f)f->Release();graph->Release();return ok;}
static DWORD WINAPI start(void*){
    bridgeStartup::Signal startup;
#ifndef _M_IX86
    return 2;
#else
    wchar_t temp[MAX_PATH],path[MAX_PATH];GetTempPathW(MAX_PATH,temp);swprintf_s(path,L"%sFlac24Override-%lu.tsv",temp,GetCurrentProcessId());output=CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(output==INVALID_HANDLE_VALUE)return 1;record("START",0,0,S_OK);
    auto co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);CLSID clsid;CLSIDFromString(L"{028A95AA-C956-4874-AD10-C657DB1B00F4}",&clsid);IID iid;CLSIDFromString(L"{1609A1C0-E0E7-4CEF-AA37-31CD15BE4ECD}",&iid);
    HMODULE module=LoadLibraryExW(L"C:\\Program Files (x86)\\Sony\\Music Center\\Sony.Earth\\OmgFlacSrc.ax",nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);IClassFactory* factory=nullptr;IUnknown* source=nullptr;HRESULT hr=E_FAIL;if(module){auto get=reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID,REFIID,void**)>(GetProcAddress(module,"DllGetClassObject"));if(get)hr=get(clsid,IID_PPV_ARGS(&factory));if(factory){hr=factory->CreateInstance(nullptr,iid,reinterpret_cast<void**>(&source));factory->Release();}}
    if(source){auto v=*reinterpret_cast<void***>(source);auto base=reinterpret_cast<ULONG_PTR>(module);auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(module);auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);bool known=nt->FileHeader.TimeDateStamp==0x63EB12C3&&reinterpret_cast<ULONG_PTR>(v[7])-base==0x1860;if(known&&MH_Initialize()==MH_OK){auto status=MH_CreateHook(v[7],reinterpret_cast<void*>(setterHook),reinterpret_cast<void**>(&original));record("CREATE_HOOK",0,0,HRESULT(status));bool ready=status==MH_OK&&setupRunHooks();
#ifdef EXPERIMENT_WAV192
ready=ready&&setupDseeHook();
#endif
if(ready){HMODULE pinned=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(v[7]),&pinned);status=MH_EnableHook(MH_ALL_HOOKS);}else status=MH_ERROR_NOT_CREATED;hr=status==MH_OK?S_OK:E_FAIL;}else hr=E_NOINTERFACE;source->Release();}
    if(SUCCEEDED(co))CoUninitialize();record(SUCCEEDED(hr)?"READY":"FAILED",0,0,hr);bridgeStatus::state.store(SUCCEEDED(hr)?bridgeStatus::State::Ready:bridgeStatus::State::Off);startup.complete(SUCCEEDED(hr));bridgeStatus::runUi(ownModule);return SUCCEEDED(hr)?0:2;
#endif
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){ownModule=module;DisableThreadLibraryCalls(module);auto t=CreateThread(nullptr,0,start,nullptr,0,nullptr);if(!t)return FALSE;CloseHandle(t);}return TRUE;}
