#include <windows.h>
#include <dsound.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cstdio>
#include <initializer_list>
int wmain(int argc,wchar_t** argv) {
    if(argc!=2 && argc!=3)return 2;
    if(argc==3) Sleep(10000); // External injection test window.
    else if(!LoadLibraryW(argv[1]))return 3;
    Sleep(3000); // Allow asynchronous hook setup in this controlled test process.
    WAVEFORMATEX f{WAVE_FORMAT_PCM,2,44100,176400,4,16,0};
    IDirectSound8* ds=nullptr;auto hr=DirectSoundCreate8(nullptr,&ds,nullptr);printf("DS create %08lx\n",ULONG(hr));
    if(ds){for(DWORD rate : {44100ul,96000ul}){f.nSamplesPerSec=rate;f.nAvgBytesPerSec=rate*4;DSBUFFERDESC d{};d.dwSize=sizeof(d);d.dwBufferBytes=rate*4;d.lpwfxFormat=&f;IDirectSoundBuffer* b=nullptr;hr=ds->CreateSoundBuffer(&d,&b,nullptr);printf("DS buffer %lu %08lx\n",rate,ULONG(hr));if(b)b->Release();}ds->Release();}
    IDirectSound* legacy=nullptr;if(SUCCEEDED(DirectSoundCreate(nullptr,&legacy,nullptr))){
        legacy->SetCooperativeLevel(GetDesktopWindow(),DSSCL_PRIORITY);
        DSBUFFERDESC primary{};primary.dwSize=sizeof(primary);primary.dwFlags=DSBCAPS_PRIMARYBUFFER;IDirectSoundBuffer* b=nullptr;
        if(SUCCEEDED(legacy->CreateSoundBuffer(&primary,&b,nullptr))){hr=b->SetFormat(&f);printf("DS legacy SetFormat %08lx\n",ULONG(hr));b->Release();}legacy->Release();
    }
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);IMMDeviceEnumerator* e=nullptr;IMMDevice* d=nullptr;IAudioClient* a=nullptr;
    hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&e));if(e)hr=e->GetDefaultAudioEndpoint(eRender,eMultimedia,&d);if(d)hr=d->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(&a));
    if(a){WAVEFORMATEX* mix=nullptr;hr=a->GetMixFormat(&mix);if(SUCCEEDED(hr)){hr=a->Initialize(AUDCLNT_SHAREMODE_SHARED,0,1000000,0,mix,nullptr);printf("WASAPI init %08lx\n",ULONG(hr));CoTaskMemFree(mix);}a->Release();}if(d)d->Release();if(e)e->Release();CoUninitialize();printf("PID %lu\n",GetCurrentProcessId());return 0;
}
