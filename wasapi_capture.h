// Optional diagnostic capture. Included only by the wasapi_pcm_probe target.
// Captures up to 20 seconds per stream, at most 16 initialized clients.
#include <vector>
#include <array>
template<class T> static bool hook(void*,int,void*,T*,const char*);
namespace capture {
struct Client {IAudioClient* audio=nullptr;IAudioRenderClient* render=nullptr;WAVEFORMATEXTENSIBLE format{};int mode=-1;BYTE* buffer=nullptr;UINT32 requested=0;HANDLE file=INVALID_HANDLE_VALUE;UINT64 frames=0;bool complete=false;};
static std::array<Client,16> clients;
static SRWLOCK mutex=SRWLOCK_INIT;
using Service=HRESULT(STDMETHODCALLTYPE*)(IAudioClient*,REFIID,void**);
using Buffer=HRESULT(STDMETHODCALLTYPE*)(IAudioRenderClient*,UINT32,BYTE**);
using Release=HRESULT(STDMETHODCALLTYPE*)(IAudioRenderClient*,UINT32,DWORD);
static Service originalService;static Buffer originalBuffer;static Release originalRelease;
static void remember(IAudioClient* audio,const WAVEFORMATEX* f,int mode,HRESULT hr){
    if(FAILED(hr)||!f||!f->nSamplesPerSec||!f->nBlockAlign)return;
    // Do not dump loopback/capture clients: association requires IAudioRenderClient.
    AcquireSRWLockExclusive(&mutex);for(auto& c:clients)if(c.audio==audio||!c.audio){if(c.file!=INVALID_HANDLE_VALUE)CloseHandle(c.file);c=Client{};c.audio=audio;c.mode=mode;memcpy(&c.format,f,min(sizeof(c.format),size_t(18)+f->cbSize));break;}ReleaseSRWLockExclusive(&mutex);
}
static HRESULT STDMETHODCALLTYPE serviceHook(IAudioClient* audio,REFIID iid,void** result){
    auto hr=originalService(audio,iid,result);if(SUCCEEDED(hr)&&result&&iid==__uuidof(IAudioRenderClient)){
        AcquireSRWLockExclusive(&mutex);for(size_t i=0;i<clients.size();++i){auto& c=clients[i];if(c.audio!=audio)continue;if(!c.render){c.render=static_cast<IAudioRenderClient*>(*result);wchar_t temp[MAX_PATH],path[MAX_PATH];GetTempPathW(MAX_PATH,temp);auto& f=c.format.Format;unsigned valid=f.wFormatTag==WAVE_FORMAT_EXTENSIBLE?c.format.Samples.wValidBitsPerSample:f.wBitsPerSample;swprintf_s(path,L"%sWASAPITap-%lu-%zu-%lu-%u-%u-%u.pcm",temp,GetCurrentProcessId(),i,f.nSamplesPerSec,f.wBitsPerSample,valid,f.nChannels);c.file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);log("capture.associate",c.file!=INVALID_HANDLE_VALUE?S_OK:HRESULT_FROM_WIN32(GetLastError()),&f,0,c.mode,nullptr,c.render);}break;}ReleaseSRWLockExclusive(&mutex);
    }return hr;
}
static HRESULT STDMETHODCALLTYPE bufferHook(IAudioRenderClient* render,UINT32 frames,BYTE** data){auto hr=originalBuffer(render,frames,data);if(SUCCEEDED(hr)&&data){AcquireSRWLockExclusive(&mutex);for(auto& c:clients)if(c.render==render){c.buffer=*data;c.requested=frames;break;}ReleaseSRWLockExclusive(&mutex);}return hr;}
static HRESULT STDMETHODCALLTYPE releaseHook(IAudioRenderClient* render,UINT32 frames,DWORD flags){
    // Copy while the app owns the buffer, but save only successfully submitted data.
    std::vector<BYTE> copy;Client* found=nullptr;UINT32 count=0;
    AcquireSRWLockExclusive(&mutex);for(auto& c:clients)if(c.render==render){found=&c;if(!c.complete&&c.file!=INVALID_HANDLE_VALUE&&c.buffer&&frames<=c.requested){UINT64 limit=UINT64(c.format.Format.nSamplesPerSec)*20;count=UINT32(min(UINT64(frames),limit-c.frames));if(count){copy.resize(size_t(count)*c.format.Format.nBlockAlign);if(!(flags&AUDCLNT_BUFFERFLAGS_SILENT))memcpy(copy.data(),c.buffer,copy.size());}}c.buffer=nullptr;c.requested=0;break;}ReleaseSRWLockExclusive(&mutex);
    auto hr=originalRelease(render,frames,flags);
    if(found&&count&&SUCCEEDED(hr)){AcquireSRWLockExclusive(&mutex);DWORD written=0;BOOL ok=WriteFile(found->file,copy.data(),DWORD(copy.size()),&written,nullptr);if(!ok||written!=copy.size()){log("capture.write_failed",HRESULT_FROM_WIN32(ok?ERROR_WRITE_FAULT:GetLastError()),&found->format.Format,0,found->mode,nullptr,render);found->complete=true;}else{found->frames+=count;if(found->frames>=UINT64(found->format.Format.nSamplesPerSec)*20){found->complete=true;log("capture.complete_20_seconds",S_OK,&found->format.Format,0,found->mode,nullptr,render);}}if(found->complete){CloseHandle(found->file);found->file=INVALID_HANDLE_VALUE;}ReleaseSRWLockExclusive(&mutex);}
    return hr;
}
static bool setup(IAudioClient* audio){
    bool ok=hook(audio,14,reinterpret_cast<void*>(serviceHook),&originalService,"hook.capture.GetService");WAVEFORMATEX* mix=nullptr;IAudioRenderClient* render=nullptr;auto hr=audio->GetMixFormat(&mix);if(SUCCEEDED(hr))hr=audio->Initialize(AUDCLNT_SHAREMODE_SHARED,0,1000000,0,mix,nullptr);if(SUCCEEDED(hr))hr=audio->GetService(IID_PPV_ARGS(&render));log("setup.capture.renderer",hr,mix);if(render){ok=hook(render,3,reinterpret_cast<void*>(bufferHook),&originalBuffer,"hook.capture.GetBuffer")&&ok;ok=hook(render,4,reinterpret_cast<void*>(releaseHook),&originalRelease,"hook.capture.ReleaseBuffer")&&ok;render->Release();}else ok=false;CoTaskMemFree(mix);return ok;
}
}
