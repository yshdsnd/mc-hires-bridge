#include <windows.h>
#include <dsound.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <intrin.h>
#include <cstdio>
#include "MinHook.h"

static SRWLOCK lock = SRWLOCK_INIT;
static HANDLE logFile = INVALID_HANDLE_VALUE;
static void log(const char* api, HRESULT hr, const WAVEFORMATEX* f, DWORD flags=0, int mode=-1, void* caller=nullptr, const void* object=nullptr, DWORD bytes=0) {
    SYSTEMTIME t; GetSystemTime(&t);
    wchar_t module[MAX_PATH]=L""; HMODULE m=nullptr;
    if(caller && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(caller), &m)) GetModuleFileNameW(m,module,MAX_PATH);
    char sub[64]="", line[2048]; unsigned valid=0, mask=0;
    if(f && f->wFormatTag==WAVE_FORMAT_EXTENSIBLE && f->cbSize>=22) {
        auto e=reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f); valid=e->Samples.wValidBitsPerSample; mask=e->dwChannelMask;
        sprintf_s(sub,"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",e->SubFormat.Data1,e->SubFormat.Data2,e->SubFormat.Data3,e->SubFormat.Data4[0],e->SubFormat.Data4[1],e->SubFormat.Data4[2],e->SubFormat.Data4[3],e->SubFormat.Data4[4],e->SubFormat.Data4[5],e->SubFormat.Data4[6],e->SubFormat.Data4[7]);
    }
    int n=sprintf_s(line,"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\t%lu\t%lu\t%u\t%s\t0x%08lx\t%p\t%lu\t%d\t0x%08lx\t%d\t%u\t%lu\t%u\t%u\t%u\t%lu\t%u\t%u\t0x%x\t%s\t%ls\r\n",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,GetCurrentProcessId(),GetCurrentThreadId(),unsigned(sizeof(void*)*8),api,ULONG(hr),object,bytes,mode,flags,f?1:0,f?f->wFormatTag:0,f?f->nSamplesPerSec:0,f?f->wBitsPerSample:0,f?f->nChannels:0,f?f->nBlockAlign:0,f?f->nAvgBytesPerSec:0,f?f->cbSize:0,valid,mask,sub,module);
    AcquireSRWLockExclusive(&lock); DWORD written; if(n>0 && logFile!=INVALID_HANDLE_VALUE) WriteFile(logFile,line,n,&written,nullptr); ReleaseSRWLockExclusive(&lock);
}
#ifdef ENABLE_WASAPI_PCM_CAPTURE
#include "wasapi_capture.h"
#endif
using Create = HRESULT (WINAPI*)(IDirectSound*,LPCDSBUFFERDESC,LPDIRECTSOUNDBUFFER*,LPUNKNOWN);
using Format = HRESULT (WINAPI*)(IDirectSoundBuffer*,LPCWAVEFORMATEX);
using Init = HRESULT (WINAPI*)(IAudioClient*,AUDCLNT_SHAREMODE,DWORD,REFERENCE_TIME,REFERENCE_TIME,const WAVEFORMATEX*,LPCGUID);
using Init3 = HRESULT (WINAPI*)(IAudioClient3*,DWORD,UINT32,const WAVEFORMATEX*,LPCGUID);
static Create originalCreate; static Format originalFormat; static Init originalInit; static Init3 originalInit3;
static HRESULT WINAPI createHook(IDirectSound* s,LPCDSBUFFERDESC d,LPDIRECTSOUNDBUFFER* b,LPUNKNOWN u) {
    auto hr=originalCreate(s,d,b,u); log("IDirectSound::CreateSoundBuffer",hr,d?d->lpwfxFormat:nullptr,d?d->dwFlags:0,-1,_ReturnAddress(),SUCCEEDED(hr)&&b?*b:nullptr,d?d->dwBufferBytes:0); return hr;
}
static HRESULT WINAPI formatHook(IDirectSoundBuffer* s,LPCWAVEFORMATEX f) { auto hr=originalFormat(s,f);log("IDirectSoundBuffer::SetFormat",hr,f,0,-1,_ReturnAddress(),s);return hr; }
static HRESULT WINAPI initHook(IAudioClient* s,AUDCLNT_SHAREMODE mode,DWORD flags,REFERENCE_TIME duration,REFERENCE_TIME period,const WAVEFORMATEX* f,LPCGUID guid) { auto hr=originalInit(s,mode,flags,duration,period,f,guid);log("IAudioClient::Initialize",hr,f,flags,mode,_ReturnAddress(),s);
#ifdef ENABLE_WASAPI_PCM_CAPTURE
    capture::remember(s,f,mode,hr);
#endif
    return hr; }
static HRESULT WINAPI init3Hook(IAudioClient3* s,DWORD flags,UINT32 period,const WAVEFORMATEX* f,LPCGUID guid) { auto hr=originalInit3(s,flags,period,f,guid);log("IAudioClient3::InitializeSharedAudioStream",hr,f,flags,0,_ReturnAddress(),s);
#ifdef ENABLE_WASAPI_PCM_CAPTURE
    capture::remember(s,f,0,hr);
#endif
    return hr; }
template<class T> static bool hook(void* object,int slot,void* detour,T* original,const char* name) {
    auto status=MH_CreateHook((*reinterpret_cast<void***>(object))[slot],detour,reinterpret_cast<void**>(original));
    log(name,HRESULT(status),nullptr); return status==MH_OK;
}
static DWORD WINAPI worker(void*) {
    wchar_t path[MAX_PATH], file[MAX_PATH]; GetTempPathW(MAX_PATH,path); swprintf_s(file,L"%sAudioFormatProbe-%lu.tsv",path,GetCurrentProcessId());
    logFile=CreateFileW(file,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(logFile==INVALID_HANDLE_VALUE) return 1;
    const char* header="utc\tpid\ttid\tarch\tapi\thresult_or_hook_status\tobject\tbuffer_bytes\tshare_mode\tflags\thas_format\ttag\trate\tbits\tchannels\tblock_align\tavg_bytes_sec\tcb_size\tvalid_bits\tchannel_mask\tsubformat\tcaller_module\r\n"; DWORD n; WriteFile(logFile,header,DWORD(strlen(header)),&n,nullptr);
    if(MH_Initialize()!=MH_OK) {log("ERROR.MH_Initialize",E_FAIL,nullptr);return 2;}
    bool dsOK=false, wasOK=false;
    IDirectSound8* ds=nullptr;
    auto hr=DirectSoundCreate8(nullptr,&ds,nullptr); log("setup.DirectSoundCreate8",hr,nullptr);
    if(SUCCEEDED(hr)) {
        dsOK=hook(ds,3,reinterpret_cast<void*>(createHook),&originalCreate,"hook.CreateSoundBuffer");
        DSBUFFERDESC desc{};desc.dwSize=sizeof(desc);desc.dwFlags=DSBCAPS_PRIMARYBUFFER;
        IDirectSoundBuffer* buffer=nullptr; hr=ds->CreateSoundBuffer(&desc,&buffer,nullptr);
        if(SUCCEEDED(hr)) {dsOK=hook(buffer,14,reinterpret_cast<void*>(formatHook),&originalFormat,"hook.SetFormat")&&dsOK;buffer->Release();}
        else {log("setup.primary_buffer",hr,nullptr);dsOK=false;} ds->Release();
    }
    auto co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    IMMDeviceEnumerator* enumerator=nullptr;IMMDevice* device=nullptr;IAudioClient* audio=nullptr;
    hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator));
    if(SUCCEEDED(hr)) hr=enumerator->GetDefaultAudioEndpoint(eRender,eMultimedia,&device);
    if(SUCCEEDED(hr)) hr=device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(&audio));
    log("setup.IAudioClient",hr,nullptr);
    if(audio) {
        wasOK=hook(audio,3,reinterpret_cast<void*>(initHook),&originalInit,"hook.Initialize");
        IAudioClient3* a3=nullptr;if(SUCCEEDED(audio->QueryInterface(IID_PPV_ARGS(&a3)))) {hook(a3,20,reinterpret_cast<void*>(init3Hook),&originalInit3,"hook.InitializeSharedAudioStream");a3->Release();}
#ifdef ENABLE_WASAPI_PCM_CAPTURE
        wasOK=capture::setup(audio)&&wasOK;
#endif
        audio->Release();
    }
    if(device) device->Release();if(enumerator) enumerator->Release();if(SUCCEEDED(co)) CoUninitialize();
    auto status=MH_EnableHook(MH_ALL_HOOKS);log("hooks.enable",HRESULT(status),nullptr);
    log(dsOK&&wasOK&&status==MH_OK?"READY":"PARTIAL_OR_FAILED",S_OK,nullptr);
    // Remain loaded until process exit. No live detach while callbacks may be active.
    return 0;
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) {DisableThreadLibraryCalls(instance);HANDLE thread=CreateThread(nullptr,0,worker,nullptr,0,nullptr);if(thread)CloseHandle(thread);else return FALSE;}return TRUE;
}
