#pragma once
#include <windows.h>
#include <dshow.h>
#include <wrl/client.h>
#include <vector>
#include <cstring>
#include <memory>

// Experimental stopped-graph rewrite. Never removes Sony filter objects or
// fabricates completion events. The shipping launcher does not enable this.
namespace wav192 {
using Microsoft::WRL::ComPtr;
inline const CLSID parserId={0xd9ce8cdc,0xb4e2,0x45b6,{0x8e,0xe9,0x09,0xde,0x2b,0x40,0x6e,0x60}};
inline const CLSID standardParserId={0xd51bd5a1,0x7548,0x11cf,{0xa5,0x20,0x00,0x80,0xc7,0x7e,0xf5,0x8a}};
inline bool isParser(REFCLSID id){return id==parserId||id==standardParserId;}
inline const CLSID seamlessId={0x6af73f1f,0x1266,0x46e6,{0x9a,0x36,0xee,0x75,0xe4,0xba,0x10,0xbb}};
inline const CLSID dseeId={0xa481d160,0xab4d,0x4b97,{0x93,0xfe,0xc1,0xf4,0xb4,0xb7,0x81,0xdf}};
inline const CLSID eqId={0x5c968488,0x5e35,0x4cfe,{0x98,0xa1,0xa5,0x1c,0x83,0x87,0x97,0xdb}};
struct Type {
 AM_MEDIA_TYPE value{};
 Type()=default;Type(const Type&)=delete;Type& operator=(const Type&)=delete;
 ~Type(){if(value.cbFormat)CoTaskMemFree(value.pbFormat);if(value.pUnk)value.pUnk->Release();}
};
struct Edge {ComPtr<IPin> out,in;Type type;};
enum class Outcome {Skipped,AlreadyNative,Applied,Restored,RestoreFailed};
struct Result {Outcome outcome=Outcome::Skipped;HRESULT change=S_FALSE,restore=S_OK;};
inline ComPtr<IPin> uniquePin(IBaseFilter* filter,PIN_DIRECTION wanted) {
 ComPtr<IEnumPins> pins;ComPtr<IPin> found;
 if(FAILED(filter->EnumPins(&pins)))return {};
 ComPtr<IPin> p;while(pins->Next(1,&p,nullptr)==S_OK){PIN_DIRECTION d{};
  if(FAILED(p->QueryDirection(&d)))return {};
  if(d==wanted){if(found)return {};found=p;}p.Reset();
 }return found;
}
inline bool knownModule(IBaseFilter* filter,const wchar_t* name,DWORD timestamp) {
 auto fn=(*reinterpret_cast<void***>(filter))[3];HMODULE module=nullptr;
 if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(fn),&module))return false;
 wchar_t path[MAX_PATH]{};if(!GetModuleFileNameW(module,path,MAX_PATH))return false;
 auto file=wcsrchr(path,L'\\');if(_wcsicmp(file?file+1:path,name))return false;
 auto base=reinterpret_cast<BYTE*>(module);auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
 if(dos->e_magic!=IMAGE_DOS_SIGNATURE)return false;
 auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
 return nt->Signature==IMAGE_NT_SIGNATURE&&nt->FileHeader.Machine==IMAGE_FILE_MACHINE_I386&&(!timestamp||nt->FileHeader.TimeDateStamp==timestamp);
}
inline bool native(const AM_MEDIA_TYPE& mt) {
 if(mt.majortype!=MEDIATYPE_Audio||mt.formattype!=FORMAT_WaveFormatEx||!mt.pbFormat||mt.cbFormat<16)return false;
 WAVEFORMATEX f{};memcpy(&f,mt.pbFormat,min(size_t(mt.cbFormat),sizeof(f)));
 return f.wFormatTag==WAVE_FORMAT_PCM&&f.nSamplesPerSec==192000&&f.wBitsPerSample==24&&f.nChannels==2&&f.nBlockAlign==6&&f.nAvgBytesPerSec==1152000;
}
inline bool connectedNative(IPin* pin) {Type mt;return SUCCEEDED(pin->ConnectionMediaType(&mt.value))&&native(mt.value)&&mt.value.subtype==MEDIASUBTYPE_PCM;}
using Connect=HRESULT (*)(IGraphBuilder*,IPin*,IPin*,const AM_MEDIA_TYPE*);
inline HRESULT connectDirect(IGraphBuilder* graph,IPin* out,IPin* in,const AM_MEDIA_TYPE* type){return graph->ConnectDirect(out,in,type);}
inline Result apply(IGraphBuilder* graph,Connect connect=connectDirect) {
 Result result;ComPtr<IMediaControl> control;OAFilterState state{};
 if(FAILED(graph->QueryInterface(IID_PPV_ARGS(&control)))||control->GetState(0,&state)!=S_OK||state!=State_Stopped)return result;
 ComPtr<IBaseFilter> parser;ComPtr<IEnumFilters> enumeration;
 if(FAILED(graph->EnumFilters(&enumeration)))return result;
 ComPtr<IBaseFilter> f;
 while(enumeration->Next(1,&f,nullptr)==S_OK){CLSID id{};
  if(SUCCEEDED(f->GetClassID(&id))&&isParser(id)){if(parser)return result;parser=f;}f.Reset();
 }
 if(!parser)return result;CLSID parserClass{};if(FAILED(parser->GetClassID(&parserClass)))return result;
 if(parserClass==parserId?!knownModule(parser.Get(),L"SonyWavParser2.ax",0x63eb12be):!knownModule(parser.Get(),L"QUARTZ.dll",0))return result;
 auto source=uniquePin(parser.Get(),PINDIR_OUTPUT);if(!source)return result;
 // Select the parser's native candidate; never promote decoded 16-bit PCM.
 Type preferred;ComPtr<IEnumMediaTypes> types;
 AM_MEDIA_TYPE* candidate=nullptr;
 if(FAILED(source->EnumMediaTypes(&types))||types->Next(1,&candidate,nullptr)!=S_OK)return result;
 preferred.value=*candidate;CoTaskMemFree(candidate);
 if(!native(preferred.value))return result;
 preferred.value.subtype=MEDIASUBTYPE_PCM;
 std::vector<std::unique_ptr<Edge>> edges;
 ComPtr<IPin> next=source,seamIn,seamOut,rendererIn;
 bool sawDsee=false,sawEq=false,sawSeamless=false,reachedRenderer=false;
 for(unsigned count=0;count<5;++count){
  auto edge=std::make_unique<Edge>();edge->out=next;
  if(FAILED(next->ConnectedTo(&edge->in))||FAILED(next->ConnectionMediaType(&edge->type.value)))return result;
  PIN_INFO info{};if(FAILED(edge->in->QueryPinInfo(&info))||!info.pFilter)return result;
  ComPtr<IBaseFilter> target;target.Attach(info.pFilter);CLSID id{};
  if(FAILED(target->GetClassID(&id)))return result;
  if(id==dseeId&&!sawDsee&&!sawSeamless&&!sawEq)sawDsee=true;
  else if(id==seamlessId&&!sawSeamless&&!sawEq){
   if(!knownModule(target.Get(),L"SeamlessFilter.ax",0x63eb12ba))return result;
   sawSeamless=true;seamIn=edge->in;seamOut=uniquePin(target.Get(),PINDIR_OUTPUT);
  }else if(id==eqId&&(sawSeamless||parserClass==standardParserId)&&!sawEq)sawEq=true;
  else if(id==CLSID_DSoundRender&&(sawSeamless||parserClass==standardParserId)){rendererIn=edge->in;reachedRenderer=true;}
  else return result;
  edges.push_back(std::move(edge));if(reachedRenderer)break;
  next=uniquePin(target.Get(),PINDIR_OUTPUT);if(!next)return result;
 }
 if(!reachedRenderer||(sawSeamless&&(!seamIn||!seamOut)))return result;
 if(!sawDsee&&!sawEq&&(!sawSeamless||connectedNative(seamIn.Get()))&&connectedNative(rendererIn.Get())){result.outcome=Outcome::AlreadyNative;result.change=S_OK;return result;}
 ComPtr<IMediaSeeking> seeking;LONGLONG position=0;
 if(FAILED(graph->QueryInterface(IID_PPV_ARGS(&seeking)))||FAILED(seeking->GetCurrentPosition(&position)))return result;
 // Snapshot is complete before disconnecting. Keep all old media types for rollback.
 HRESULT hr=S_OK;
 for(auto& e:edges){auto a=graph->Disconnect(e->out.Get());auto b=graph->Disconnect(e->in.Get());if(FAILED(a)&&SUCCEEDED(hr))hr=a;if(FAILED(b)&&SUCCEEDED(hr))hr=b;}
 if(SUCCEEDED(hr))hr=connect(graph,source.Get(),sawSeamless?seamIn.Get():rendererIn.Get(),&preferred.value);
 if(SUCCEEDED(hr)&&sawSeamless)hr=connect(graph,seamOut.Get(),rendererIn.Get(),nullptr);
 if(SUCCEEDED(hr)&&((sawSeamless&&!connectedNative(seamIn.Get()))||!connectedNative(rendererIn.Get())))hr=VFW_E_TYPE_NOT_ACCEPTED;
 if(SUCCEEDED(hr))hr=seeking->SetPositions(&position,AM_SEEKING_AbsolutePositioning,nullptr,AM_SEEKING_NoPositioning);
 result.change=hr;
 if(SUCCEEDED(hr)){result.outcome=Outcome::Applied;return result;}
 // Disconnect both ends, including partially established replacement edges.
 for(auto& e:edges){graph->Disconnect(e->out.Get());graph->Disconnect(e->in.Get());}
 HRESULT rollback=S_OK;
 for(auto& e:edges){auto r=graph->ConnectDirect(e->out.Get(),e->in.Get(),&e->type.value);if(FAILED(r)&&SUCCEEDED(rollback))rollback=r;}
 if(SUCCEEDED(rollback))rollback=seeking->SetPositions(&position,AM_SEEKING_AbsolutePositioning,nullptr,AM_SEEKING_NoPositioning);
 result.restore=rollback;result.outcome=SUCCEEDED(rollback)?Outcome::Restored:Outcome::RestoreFailed;return result;
}
} // namespace wav192
