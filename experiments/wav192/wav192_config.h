#pragma once
#include <windows.h>
namespace wav192 {
// Observed x86 interface slot 3: this pointer + nine DWORDs by value, ret 0x28.
struct DseeConfig {DWORD mode,channels,format,inputRate,outputRate,inputBits,outputBits,unknown,quality;};
static_assert(sizeof(DseeConfig)==36);
inline bool nativeConfig(const DseeConfig& requested,DseeConfig& result) {
 if(requested.mode>1||requested.channels!=2||requested.format!=16||requested.inputRate!=192000||requested.inputBits!=24||requested.outputRate!=44100||requested.outputBits!=16)return false;
 result=requested;result.mode=0;result.format=24;result.outputRate=192000;result.outputBits=24;return true;
}
}
