// Version-specific x86 runtime setting override used by the production bridge. No file patches.
#include <windows.h>
#include <dshow.h>
#include <mmreg.h>
#include <intrin.h>
#include <cstdio>
#include <map>
#include <wrl/client.h>
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
#include "wav192_hooks.inl"
#else
#include "shared_dsee.inl"
#endif
static bridgeStatus::Format pcmFormat(const AM_MEDIA_TYPE& type) {
    if(type.majortype!=MEDIATYPE_Audio || type.formattype!=FORMAT_WaveFormatEx ||
       type.cbFormat<16 || !type.pbFormat)return {};
    WAVEFORMATEX wave{};
    memcpy(&wave,type.pbFormat,min(size_t(type.cbFormat),sizeof(wave)));
    return {wave.nSamplesPerSec,wave.wBitsPerSample,wave.nChannels};
}
static void freeMediaType(AM_MEDIA_TYPE& type) {
    CoTaskMemFree(type.pbFormat);
    if(type.pUnk)type.pUnk->Release();
}
static const wchar_t* fileCodec(IBaseFilter* filter) {
    Microsoft::WRL::ComPtr<IFileSourceFilter> source;
    if(FAILED(filter->QueryInterface(IID_PPV_ARGS(&source))))return L"";
    LPOLESTR path=nullptr;
    AM_MEDIA_TYPE type{};
    const auto hr=source->GetCurFile(&path,&type);
    const wchar_t* codec=L"";
    if(SUCCEEDED(hr) && path) {
        if(const auto ext=wcsrchr(path,L'.')) {
            if(!_wcsicmp(ext,L".flac"))codec=L"FLAC";
            else if(!_wcsicmp(ext,L".wav"))codec=L"WAV";
            else if(!_wcsicmp(ext,L".mp3"))codec=L"MP3";
            else if(!_wcsicmp(ext,L".aac"))codec=L"AAC";
            else if(!_wcsicmp(ext,L".alac"))codec=L"ALAC";
            else if(!_wcsicmp(ext,L".m4a") || !_wcsicmp(ext,L".mp4"))codec=L"M4A";
        }
    }
    CoTaskMemFree(path);
    freeMediaType(type);
    return codec;
}
static void updateStatus(IUnknown* object,HRESULT result) {
    using Microsoft::WRL::ComPtr;
    bridgeStatus::Playback playback;
    bool source=false, output24=false, dseeActive=false;
    ComPtr<IFilterGraph> graph;
    ComPtr<IEnumFilters> filters;
    if(SUCCEEDED(result) && bridgeStatus::directSoundSelected() &&
       SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&graph))) && SUCCEEDED(graph->EnumFilters(&filters))) {
        playback.route=bridgeStatus::Route::Normal;
        ComPtr<IBaseFilter> filter;
        DWORD inputCodec=0;
        while(filters->Next(1,filter.ReleaseAndGetAddressOf(),nullptr)==S_OK) {
            CLSID cls{};
            if(FAILED(filter->GetClassID(&cls)))continue;
            if(sourceApplied(filter.Get()))source=true;
            if(!*playback.codec)playback.codec=fileCodec(filter.Get());
#ifndef EXPERIMENT_WAV192
            if(cls==dsee) {
                const auto config=dseeSnapshot(filter.Get()).input;
                if(config.inputRate && config.inputBits && config.channels) {
                    inputCodec=config.format;
                    playback.input={config.inputRate,WORD(config.inputBits),WORD(config.channels)};
                }
            }
#endif
            if(cls!=CLSID_DSoundRender)continue;
            ComPtr<IEnumPins> pins;
            if(FAILED(filter->EnumPins(&pins)))continue;
            ComPtr<IPin> pin;
            while(pins->Next(1,pin.ReleaseAndGetAddressOf(),nullptr)==S_OK) {
                AM_MEDIA_TYPE type{};
                if(FAILED(pin->ConnectionMediaType(&type)))continue;
                playback.output=pcmFormat(type);
                const auto format=playback.output;
                output24=supportedOutputRate(format.rate) && format.bits==24 && format.channels==2;
#ifndef EXPERIMENT_WAV192
                dseeActive=dseeFeedsRenderer(pin.Get(),type);
#endif
                freeMediaType(type);
            }
        }
        if(inputCodec==2)playback.codec=L"MP3";
        else if(inputCodec==3)playback.codec=L"AAC";
        else if(inputCodec==16)playback.codec=L"WAV";
        else if(inputCodec==101) {
            if(wcscmp(playback.codec,L"M4A")==0)playback.codec=L"ALAC";
            else if(!*playback.codec)playback.codec=L"FLAC / ALAC";
        }
        if(dseeActive)playback.route=bridgeStatus::Route::Dsee;
        else if(source && output24) {
            playback.route=bridgeStatus::Route::Flac24;
            if(!*playback.codec)playback.codec=L"FLAC";
            playback.input=playback.output;
        }
    }
    bridgeStatus::publish(playback);
    const auto next=((source&&output24)||dseeActive)?bridgeStatus::State::On:bridgeStatus::State::Off;
    bridgeStatus::state.store(next);
    record(next==bridgeStatus::State::On?"STATUS_ON":"STATUS_OFF",0,0,result);
}
static HRESULT STDMETHODCALLTYPE runHook(IMediaControl* self){
#ifdef EXPERIMENT_WAV192
    WavScope scope;if(scope.outer){auto check=prepareWav(self);if(FAILED(check))return check;}
#else
    DseeScope scope;if(scope.outer){auto check=prepareDirect(self);if(FAILED(check))return check;}
#endif
    auto hr=originalRun(self);updateStatus(self,hr);return hr;}
static HRESULT STDMETHODCALLTYPE filterRunHook(IMediaFilter* self,REFERENCE_TIME time){
#ifdef EXPERIMENT_WAV192
    WavScope scope;if(scope.outer){auto check=prepareWav(self);if(FAILED(check))return check;}
#else
    DseeScope scope;if(scope.outer){auto check=prepareDirect(self);if(FAILED(check))return check;}
#endif
    auto hr=originalFilterRun(self,time);updateStatus(self,hr);return hr;}
static bool setupRunHooks(){IGraphBuilder* graph=nullptr;if(FAILED(CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&graph))))return false;bool ok=false;IMediaControl* c=nullptr;IMediaFilter* f=nullptr;if(SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&c)))&&SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&f)))){auto a=(*reinterpret_cast<void***>(c))[7],b=(*reinterpret_cast<void***>(f))[6];HMODULE pinned=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(a),&pinned);ok=MH_CreateHook(a,reinterpret_cast<void*>(runHook),reinterpret_cast<void**>(&originalRun))==MH_OK&&MH_CreateHook(b,reinterpret_cast<void*>(filterRunHook),reinterpret_cast<void**>(&originalFilterRun))==MH_OK;}
#ifdef EXPERIMENT_WAV192
if(ok){auto pause=(*reinterpret_cast<void***>(c))[8],filterPause=(*reinterpret_cast<void***>(f))[5];ok=MH_CreateHook(pause,reinterpret_cast<void*>(pauseHook),reinterpret_cast<void**>(&originalPause))==MH_OK&&MH_CreateHook(filterPause,reinterpret_cast<void*>(filterPauseHook),reinterpret_cast<void**>(&originalFilterPause))==MH_OK;}
#endif
#ifndef EXPERIMENT_WAV192
if(ok && (!setupDseePause(c,f) || !setupSharedDsee()))record("DSEE_UNAVAILABLE",0,0,S_FALSE);
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
