#include <windows.h>
#include <dsound.h>
#include <mmreg.h>
#include <cstdio>
#include "MinHook.h"
using Unlock=HRESULT(WINAPI*)(IDirectSoundBuffer*,LPVOID,DWORD,LPVOID,DWORD);
static Unlock original;
static SRWLOCK mutex=SRWLOCK_INIT;
struct Stream {IDirectSoundBuffer* object;DWORD rate;WORD bits;HANDLE file;DWORD bytes;DWORD cap;};
static Stream streams[32]{};static unsigned count=0;
static HRESULT WINAPI unlockHook(IDirectSoundBuffer* buffer,LPVOID p1,DWORD b1,LPVOID p2,DWORD b2){
    WAVEFORMATEXTENSIBLE fmt{};DWORD needed=0;auto hr=buffer->GetFormat(&fmt.Format,sizeof(fmt),&needed);
    if(SUCCEEDED(hr)&&fmt.Format.nBlockAlign&&fmt.Format.nAvgBytesPerSec){AcquireSRWLockExclusive(&mutex);Stream* stream=nullptr;
        for(unsigned i=0;i<count;++i)if(streams[i].object==buffer&&streams[i].rate==fmt.Format.nSamplesPerSec&&streams[i].bits==fmt.Format.wBitsPerSample){stream=&streams[i];break;}
        if(!stream&&count<32){stream=&streams[count++];stream->object=buffer;stream->rate=fmt.Format.nSamplesPerSec;stream->bits=fmt.Format.wBitsPerSample;stream->cap=fmt.Format.nAvgBytesPerSec*20;
            wchar_t temp[MAX_PATH],path[MAX_PATH];GetTempPathW(MAX_PATH,temp);swprintf_s(path,L"%sPCMTap-%lu-%u-%lu-%u-%u.pcm",temp,GetCurrentProcessId(),count,stream->rate,stream->bits,fmt.Format.nChannels);stream->file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        }
        if(stream&&stream->file!=INVALID_HANDLE_VALUE){DWORD written=0;DWORD n=b1<(stream->cap-stream->bytes)?b1:(stream->cap-stream->bytes);if(n&&p1&&WriteFile(stream->file,p1,n,&written,nullptr))stream->bytes+=written;n=b2<(stream->cap-stream->bytes)?b2:(stream->cap-stream->bytes);if(n&&p2&&WriteFile(stream->file,p2,n,&written,nullptr))stream->bytes+=written;}
        ReleaseSRWLockExclusive(&mutex);
    }return original(buffer,p1,b1,p2,b2);
}
static DWORD WINAPI start(void*){IDirectSound8* ds=nullptr;auto hr=DirectSoundCreate8(nullptr,&ds,nullptr);if(FAILED(hr))return 1;WAVEFORMATEX f{WAVE_FORMAT_PCM,2,96000,384000,4,16,0};DSBUFFERDESC d{};d.dwSize=sizeof(d);d.dwBufferBytes=38400;d.lpwfxFormat=&f;IDirectSoundBuffer* b=nullptr;hr=ds->CreateSoundBuffer(&d,&b,nullptr);if(SUCCEEDED(hr)){void* target=(*reinterpret_cast<void***>(b))[19];HMODULE module;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN|GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(target),&module);if(MH_Initialize()==MH_OK&&MH_CreateHook(target,reinterpret_cast<void*>(unlockHook),reinterpret_cast<void**>(&original))==MH_OK)MH_EnableHook(target);b->Release();}ds->Release();return SUCCEEDED(hr)?0:2;}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){DisableThreadLibraryCalls(instance);HANDLE thread=CreateThread(nullptr,0,start,nullptr,0,nullptr);if(!thread)return FALSE;CloseHandle(thread);}return TRUE;}
