// Observation only: record negotiation before the first source connection.
#include <windows.h>
#include <dshow.h>
#include <mmreg.h>
#include <cstdio>
#include <cstdarg>
#include <atomic>
#include <array>
#include <utility>
#include <vector>
#include "MinHook.h"

static HANDLE logFile=INVALID_HANDLE_VALUE;
static SRWLOCK logLock=SRWLOCK_INIT;
static std::atomic<unsigned> sequence{0};
static thread_local bool internal=false;
static thread_local unsigned parent=0;
static void log(const wchar_t* event,const wchar_t* format=L"",...) {
    wchar_t msg[6500],line[7200];va_list args;va_start(args,format);
    _vsnwprintf_s(msg,_countof(msg),_TRUNCATE,format,args);va_end(args);
    for(auto p=msg;*p;++p)if(*p==L'\t'||*p==L'\r'||*p==L'\n')*p=L' ';
    SYSTEMTIME t;GetSystemTime(&t);AcquireSRWLockExclusive(&logLock);
    _snwprintf_s(line,_countof(line),_TRUNCATE,L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\t%lu\t%lu\t%u\t%s\t%s\r\n",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,GetCurrentProcessId(),GetCurrentThreadId(),++sequence,event,msg);
    char utf8[30000];int n=WideCharToMultiByte(CP_UTF8,0,line,-1,utf8,sizeof(utf8),nullptr,nullptr);DWORD written;if(n>1)WriteFile(logFile,utf8,n-1,&written,nullptr);ReleaseSRWLockExclusive(&logLock);
}
static void freeType(AM_MEDIA_TYPE& mt){if(mt.cbFormat)CoTaskMemFree(mt.pbFormat);if(mt.pUnk)mt.pUnk->Release();mt={};}
static void type(const wchar_t* event,unsigned call,void* object,const AM_MEDIA_TYPE* mt,HRESULT hr=S_OK){
    WAVEFORMATEXTENSIBLE f{};if(mt&&mt->formattype==FORMAT_WaveFormatEx&&mt->pbFormat)memcpy(&f,mt->pbFormat,min(size_t(mt->cbFormat),sizeof(f)));
    log(event,L"call=%u object=%p has_type=%d hr=%08lx rate=%lu bits=%u valid_bits=%u channels=%u tag=%u align=%u bytes=%lu",call,object,mt?1:0,ULONG(hr),f.Format.nSamplesPerSec,f.Format.wBitsPerSample,f.Samples.wValidBitsPerSample,f.Format.nChannels,f.Format.wFormatTag,f.Format.nBlockAlign,mt?mt->cbFormat:0);
}
static void stack(unsigned call){void* frames[16];auto count=CaptureStackBackTrace(1,16,frames,nullptr);for(USHORT i=0;i<count;++i){HMODULE m=nullptr;wchar_t path[MAX_PATH]=L"?";GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(frames[i]),&m);if(m)GetModuleFileNameW(m,path,MAX_PATH);auto name=wcsrchr(path,L'\\');log(L"STACK",L"call=%u frame=%u module=%s offset=%p",call,i,name?name+1:path,reinterpret_cast<void*>(reinterpret_cast<ULONG_PTR>(frames[i])-reinterpret_cast<ULONG_PTR>(m)));}}
struct Scope {unsigned call,previous;Scope(const wchar_t* api,void* self,void* peer=nullptr):call(++sequence),previous(parent){parent=call;log(L"ENTER",L"call=%u parent=%u api=%s self=%p peer=%p",call,previous,api,self,peer);stack(call);}~Scope(){parent=previous;}};
static void pinInfo(IPin* p){bool was=internal;internal=true;PIN_INFO pi{};if(p&&SUCCEEDED(p->QueryPinInfo(&pi))){CLSID id{};wchar_t cls[40]=L"";if(pi.pFilter){pi.pFilter->GetClassID(&id);StringFromGUID2(id,cls,40);}log(L"PIN",L"pin=%p filter=%p clsid=%s name=%s direction=%d",p,pi.pFilter,cls,pi.achName,pi.dir);if(pi.pFilter)pi.pFilter->Release();}internal=was;}
static void advertised(IPin* p,const wchar_t* event,unsigned call){bool was=internal;internal=true;IEnumMediaTypes* e=nullptr;if(SUCCEEDED(p->EnumMediaTypes(&e))&&e){AM_MEDIA_TYPE* mt=nullptr;unsigned count=0;while(count++<16&&e->Next(1,&mt,nullptr)==S_OK){type(event,call,p,mt);freeType(*mt);CoTaskMemFree(mt);}e->Release();}internal=was;}

// Each distinct implementation address gets its own trampoline; shared base-class
// implementations are installed only once. Pool size is bounded and setup fails closed.
template<class Tag,class Fn,size_t N=32> struct Pool {
    inline static Fn originals[N]{};
    inline static void* addresses[N]{};
    inline static size_t used=0;
    template<size_t... I> static auto wrappers(std::index_sequence<I...>){return std::array<Fn,N>{&Tag::template hook<I>...};}
    static bool add(void* object,int slot,const wchar_t* name){void* addr=(*reinterpret_cast<void***>(object))[slot];for(size_t i=0;i<used;++i)if(addresses[i]==addr)return true;if(used==N)return false;auto handlers=wrappers(std::make_index_sequence<N>{});auto result=MH_CreateHook(addr,reinterpret_cast<void*>(handlers[used]),reinterpret_cast<void**>(&originals[used]));log(L"SETUP",L"api=%s address=%p slot=%d status=%d",name,addr,slot,result);if(result!=MH_OK)return false;HMODULE m=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(addr),&m);addresses[used++]=addr;return true;}
};
using PinConnect=HRESULT(STDMETHODCALLTYPE*)(IPin*,IPin*,const AM_MEDIA_TYPE*);
struct ConnectTag;using ConnectPool=Pool<ConnectTag,PinConnect>;
struct ConnectTag {template<size_t I>static HRESULT STDMETHODCALLTYPE hook(IPin* self,IPin* peer,const AM_MEDIA_TYPE* mt){if(internal)return ConnectPool::originals[I](self,peer,mt);Scope s(L"IPin.Connect",self,peer);pinInfo(self);pinInfo(peer);type(L"REQUEST",s.call,self,mt);advertised(self,L"BEFORE_PREFERRED",s.call);auto hr=ConnectPool::originals[I](self,peer,mt);AM_MEDIA_TYPE actual{};bool was=internal;internal=true;auto r=self->ConnectionMediaType(&actual);internal=was;type(L"CONNECTED",s.call,self,SUCCEEDED(r)?&actual:nullptr,hr);freeType(actual);return hr;}};
struct ReceiveTag;using ReceivePool=Pool<ReceiveTag,PinConnect>;
struct ReceiveTag {template<size_t I>static HRESULT STDMETHODCALLTYPE hook(IPin* self,IPin* peer,const AM_MEDIA_TYPE* mt){if(internal)return ReceivePool::originals[I](self,peer,mt);Scope s(L"IPin.ReceiveConnection",self,peer);pinInfo(self);pinInfo(peer);type(L"REQUEST",s.call,self,mt);auto hr=ReceivePool::originals[I](self,peer,mt);type(L"RETURN",s.call,self,mt,hr);return hr;}};
using Accept=HRESULT(STDMETHODCALLTYPE*)(IPin*,const AM_MEDIA_TYPE*);
struct AcceptTag;using AcceptPool=Pool<AcceptTag,Accept>;
struct AcceptTag {template<size_t I>static HRESULT STDMETHODCALLTYPE hook(IPin* self,const AM_MEDIA_TYPE* mt){if(internal)return AcceptPool::originals[I](self,mt);Scope s(L"IPin.QueryAccept",self);type(L"REQUEST",s.call,self,mt);auto hr=AcceptPool::originals[I](self,mt);type(L"RETURN",s.call,self,mt,hr);return hr;}};
using Next=HRESULT(STDMETHODCALLTYPE*)(IEnumMediaTypes*,ULONG,AM_MEDIA_TYPE**,ULONG*);
struct NextTag;using NextPool=Pool<NextTag,Next>;
struct NextTag {template<size_t I>static HRESULT STDMETHODCALLTYPE hook(IEnumMediaTypes* self,ULONG n,AM_MEDIA_TYPE** types,ULONG* fetched){if(internal)return NextPool::originals[I](self,n,types,fetched);Scope s(L"IEnumMediaTypes.Next",self);auto hr=NextPool::originals[I](self,n,types,fetched);ULONG count=fetched?*fetched:(hr==S_OK?n:0);if(SUCCEEDED(hr)&&types)for(ULONG j=0;j<min(count,ULONG(32));++j)type(L"ENUM_RESULT",s.call,self,types[j],hr);log(L"NEXT_RETURN",L"call=%u hr=%08lx fetched=%lu",s.call,ULONG(hr),count);return hr;}};
using Enum=HRESULT(STDMETHODCALLTYPE*)(IPin*,IEnumMediaTypes**);
struct EnumTag;using EnumPool=Pool<EnumTag,Enum>;
struct EnumTag {template<size_t I>static HRESULT STDMETHODCALLTYPE hook(IPin* self,IEnumMediaTypes** result){if(internal)return EnumPool::originals[I](self,result);Scope s(L"IPin.EnumMediaTypes",self);pinInfo(self);auto hr=EnumPool::originals[I](self,result);log(L"ENUMERATOR",L"call=%u pin=%p enum=%p hr=%08lx",s.call,self,SUCCEEDED(hr)&&result?*result:nullptr,ULONG(hr));return hr;}};
using Load=HRESULT(STDMETHODCALLTYPE*)(IFileSourceFilter*,LPCOLESTR,const AM_MEDIA_TYPE*);
struct LoadTag;using LoadPool=Pool<LoadTag,Load>;
struct LoadTag {template<size_t I>static HRESULT STDMETHODCALLTYPE hook(IFileSourceFilter* self,LPCOLESTR path,const AM_MEDIA_TYPE* mt){if(internal)return LoadPool::originals[I](self,path,mt);Scope s(L"IFileSourceFilter.Load",self);type(L"LOAD_REQUEST",s.call,self,mt);auto hr=LoadPool::originals[I](self,path,mt);log(L"LOAD_RETURN",L"call=%u hr=%08lx",s.call,ULONG(hr));IBaseFilter* f=nullptr;bool was=internal;internal=true;if(SUCCEEDED(self->QueryInterface(IID_PPV_ARGS(&f)))){IEnumPins* e=nullptr;if(SUCCEEDED(f->EnumPins(&e))){IPin* p=nullptr;while(e->Next(1,&p,nullptr)==S_OK){pinInfo(p);advertised(p,L"AFTER_LOAD_PREFERRED",s.call);p->Release();}e->Release();}f->Release();}internal=was;return hr;}};
using QI=HRESULT(STDMETHODCALLTYPE*)(IUnknown*,REFIID,void**);
struct QITag;using QIPool=Pool<QITag,QI>;
struct QITag {template<size_t I>static HRESULT STDMETHODCALLTYPE hook(IUnknown* self,REFIID iid,void** result){auto hr=QIPool::originals[I](self,iid,result);if(!internal){wchar_t id[40];StringFromGUID2(iid,id,40);log(L"QI",L"parent=%u self=%p iid=%s result=%p hr=%08lx",parent,self,id,SUCCEEDED(hr)&&result?*result:nullptr,ULONG(hr));}return hr;}};
using Direct=HRESULT(STDMETHODCALLTYPE*)(IFilterGraph*,IPin*,IPin*,const AM_MEDIA_TYPE*);
struct DirectTag;using DirectPool=Pool<DirectTag,Direct>;
struct DirectTag {template<size_t I>static HRESULT STDMETHODCALLTYPE hook(IFilterGraph* self,IPin* out,IPin* in,const AM_MEDIA_TYPE* mt){if(internal)return DirectPool::originals[I](self,out,in,mt);Scope s(L"IFilterGraph.ConnectDirect",self);pinInfo(out);pinInfo(in);type(L"GRAPH_REQUEST",s.call,out,mt);auto hr=DirectPool::originals[I](self,out,in,mt);log(L"GRAPH_RETURN",L"call=%u hr=%08lx",s.call,ULONG(hr));return hr;}};

static void sourcePreferred(IUnknown* object,const wchar_t* event,unsigned call){bool was=internal;internal=true;IBaseFilter* f=nullptr;if(SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&f)))){IEnumPins* e=nullptr;if(SUCCEEDED(f->EnumPins(&e))){IPin* p=nullptr;while(e->Next(1,&p,nullptr)==S_OK){advertised(p,event,call);p->Release();}e->Release();}f->Release();}internal=was;}
// Only these two scalar setters have been inspected (x86 stdcall, ret 8).
// Their official names are unknown. Never infer signatures for other slots.
using Scalar=HRESULT(STDMETHODCALLTYPE*)(IUnknown*,DWORD);
struct ScalarTag;using ScalarPool=Pool<ScalarTag,Scalar>;
struct ScalarTag {template<size_t I>static HRESULT STDMETHODCALLTYPE hook(IUnknown* self,DWORD value){if(internal)return ScalarPool::originals[I](self,value);Scope s(L"SonyFLAC.PrivateScalar",self);log(L"SCALAR_REQUEST",L"call=%u implementation=%zu value=%lu",s.call,I,value);sourcePreferred(self,L"BEFORE_SCALAR_PREFERRED",s.call);auto hr=ScalarPool::originals[I](self,value);log(L"SCALAR_RETURN",L"call=%u hr=%08lx",s.call,ULONG(hr));sourcePreferred(self,L"AFTER_SCALAR_PREFERRED",s.call);return hr;}};
static bool setupPrivate(IBaseFilter* f){
#ifdef _M_IX86
    IID iid;CLSIDFromString(L"{1609A1C0-E0E7-4CEF-AA37-31CD15BE4ECD}",&iid);IUnknown* p=nullptr;if(FAILED(f->QueryInterface(iid,reinterpret_cast<void**>(&p))))return true;
    auto v=*reinterpret_cast<void***>(p);HMODULE m=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(v[4]),&m);
    auto base=reinterpret_cast<ULONG_PTR>(m);auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(m);auto nt=m?reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew):nullptr;
    bool known=nt&&nt->FileHeader.TimeDateStamp==0x63EB12C3&&reinterpret_cast<ULONG_PTR>(v[4])-base==0x16f0&&reinterpret_cast<ULONG_PTR>(v[7])-base==0x1860;
    bool ok=true;if(known){ok=ScalarPool::add(p,4,L"SonyFLAC.Private.slot4")&&ok;ok=ScalarPool::add(p,7,L"SonyFLAC.Private.slot7")&&ok;}else log(L"PRIVATE_SKIPPED",L"unrecognized_implementation=1");p->Release();return ok;
#else
    log(L"PRIVATE_SKIPPED",L"x86_only=1");return true;
#endif
}

static bool setupFilter(IBaseFilter* f){bool ok=QIPool::add(f,0,L"Filter.QueryInterface");IFileSourceFilter* source=nullptr;if(SUCCEEDED(f->QueryInterface(IID_PPV_ARGS(&source)))){ok=LoadPool::add(source,3,L"Source.Load")&&ok;source->Release();}IEnumPins* e=nullptr;if(SUCCEEDED(f->EnumPins(&e))){IPin* p=nullptr;while(e->Next(1,&p,nullptr)==S_OK){pinInfo(p);ok=ConnectPool::add(p,3,L"Pin.Connect")&&ok;ok=ReceivePool::add(p,4,L"Pin.ReceiveConnection")&&ok;ok=AcceptPool::add(p,11,L"Pin.QueryAccept")&&ok;ok=EnumPool::add(p,12,L"Pin.EnumMediaTypes")&&ok;IEnumMediaTypes* types=nullptr;if(SUCCEEDED(p->EnumMediaTypes(&types))&&types){ok=NextPool::add(types,3,L"Enum.Next")&&ok;types->Release();}p->Release();}e->Release();}return ok;}
static DWORD WINAPI start(void*){
    wchar_t temp[MAX_PATH],path[MAX_PATH];GetTempPathW(MAX_PATH,temp);swprintf_s(path,L"%sNegotiationProbe-%lu.tsv",temp,GetCurrentProcessId());logFile=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(logFile==INVALID_HANDLE_VALUE)return 1;
    log(L"START",L"observation_only=1 arch=%u",unsigned(sizeof(void*)*8));internal=true;auto co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);bool ok=SUCCEEDED(co)&&MH_Initialize()==MH_OK;
    struct Spec{const wchar_t* id;const wchar_t* module;};Spec specs[]={
        {L"{028A95AA-C956-4874-AD10-C657DB1B00F4}",L"OmgFlacSrc.ax"},
        {L"{A481D160-AB4D-4B97-93FE-C1F4B4B781DF}",L"OmgDseeHxFilter.ax"},
        {L"{6AF73F1F-1266-46E6-9A36-EE75E4BA10BB}",L"SeamlessFilter.ax"},
        {L"{5C968488-5E35-4CFE-98A1-A51C838797DB}",L"OmgEqxlFilter.ax"}};
    for(auto& spec:specs){CLSID clsid;CLSIDFromString(spec.id,&clsid);wchar_t dllPath[MAX_PATH];swprintf_s(dllPath,L"C:\\Program Files (x86)\\Sony\\Music Center\\Sony.Earth\\%s",spec.module);HMODULE dll=LoadLibraryExW(dllPath,nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);IClassFactory* factory=nullptr;IBaseFilter* f=nullptr;HRESULT hr=E_FAIL;if(dll){auto get=reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID,REFIID,void**)>(GetProcAddress(dll,"DllGetClassObject"));if(get)hr=get(clsid,IID_PPV_ARGS(&factory));if(factory){hr=factory->CreateInstance(nullptr,IID_PPV_ARGS(&f));factory->Release();}}log(L"CREATE_REFERENCE",L"module=%s hr=%08lx",spec.module,ULONG(hr));if(f){ok=setupFilter(f)&&ok;ok=setupPrivate(f)&&ok;f->Release();}else ok=false;}
    IGraphBuilder* graph=nullptr;if(SUCCEEDED(CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&graph)))){ok=DirectPool::add(graph,7,L"Graph.ConnectDirect")&&ok;graph->Release();}else ok=false;
    if(ok){auto r=MH_EnableHook(MH_ALL_HOOKS);log(L"ENABLE",L"status=%d",r);ok=r==MH_OK;}
    if(SUCCEEDED(co))CoUninitialize();internal=false;log(ok?L"READY":L"FAILED");return ok?0:2;
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){DisableThreadLibraryCalls(module);auto t=CreateThread(nullptr,0,start,nullptr,0,nullptr);if(!t)return FALSE;CloseHandle(t);}return TRUE;}
