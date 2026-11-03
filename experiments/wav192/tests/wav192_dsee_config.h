#pragma once
#include "wav192_config.h"
// Configure a filter owned by this diagnostic, never a filter in Music Center.
// Guard the private ABI against the one independently inspected implementation.
inline HRESULT configureTestDsee(IBaseFilter* filter,bool downsample) {
 GUID iid;CLSIDFromString(L"{6AF76DBE-8634-4FD3-93ED-982EDE1BFDD5}",&iid);
 Microsoft::WRL::ComPtr<IUnknown> control;
 auto hr=filter->QueryInterface(iid,reinterpret_cast<void**>(control.GetAddressOf()));if(FAILED(hr))return hr;
 auto fn=(*reinterpret_cast<void***>(control.Get()))[3];HMODULE module=nullptr;
 if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(fn),&module))return E_FAIL;
 auto base=reinterpret_cast<BYTE*>(module);auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
 if(nt->FileHeader.TimeDateStamp!=0x63eb12c1||reinterpret_cast<BYTE*>(fn)-base!=0x36ca)return E_NOINTERFACE;
 wav192::DseeConfig requested{0,2,16,192000,44100,24,16,0,0},adjusted{};
 if(!wav192::nativeConfig(requested,adjusted))return E_FAIL;
 using Set=HRESULT(STDMETHODCALLTYPE*)(IUnknown*,wav192::DseeConfig);
 return reinterpret_cast<Set>(fn)(control.Get(),downsample?requested:adjusted);
}
