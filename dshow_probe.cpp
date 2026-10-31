#include <windows.h>
#include <dshow.h>
#include <mmreg.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <atomic>
#include <vector>
#include <string>
#include "MinHook.h"

static HANDLE file=INVALID_HANDLE_VALUE;
static SRWLOCK outputLock=SRWLOCK_INIT;
static std::atomic<unsigned> serial{0};
static thread_local bool observing=false;
static void write(const wchar_t* event,const wchar_t* format=L"",...) {
    wchar_t text[7000],line[8000];va_list args;va_start(args,format);
    _vsnwprintf_s(text,_countof(text),_TRUNCATE,format,args);va_end(args);
    for(auto p=text;*p;++p)if(*p==L'\t'||*p==L'\r'||*p==L'\n')*p=L' ';
    SYSTEMTIME t;GetSystemTime(&t);
    _snwprintf_s(line,_countof(line),_TRUNCATE,L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ\t%lu\t%lu\t%s\t%s\r\n",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,GetCurrentProcessId(),GetCurrentThreadId(),event,text);
    char utf8[32000];int n=WideCharToMultiByte(CP_UTF8,0,line,-1,utf8,sizeof(utf8),nullptr,nullptr);
    AcquireSRWLockExclusive(&outputLock);DWORD written;if(n>1&&file!=INVALID_HANDLE_VALUE)WriteFile(file,utf8,n-1,&written,nullptr);ReleaseSRWLockExclusive(&outputLock);
}
static void guidText(REFGUID guid,wchar_t* output){StringFromGUID2(guid,output,40);}
static void moduleOf(void* address,wchar_t* path) {
    HMODULE module=nullptr;*path=0;
    if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(address),&module))GetModuleFileNameW(module,path,MAX_PATH);
}
static void freeType(AM_MEDIA_TYPE& mt){if(mt.cbFormat)CoTaskMemFree(mt.pbFormat);if(mt.pUnk)mt.pUnk->Release();mt={};}
static void media(unsigned id,const wchar_t* event,IPin* pin,HRESULT hr,const AM_MEDIA_TYPE* mt,int candidate=-1) {
    if(!mt){write(event,L"snapshot=%u pin=%p hr=0x%08lx",id,pin,ULONG(hr));return;}
    wchar_t major[40],sub[40],fmt[40],waveSub[40]=L"";guidText(mt->majortype,major);guidText(mt->subtype,sub);guidText(mt->formattype,fmt);
    WAVEFORMATEXTENSIBLE storage{};bool wave=mt->formattype==FORMAT_WaveFormatEx && mt->pbFormat && mt->cbFormat>=16;
    if(wave){memcpy(&storage,mt->pbFormat,(mt->cbFormat<sizeof(storage)?mt->cbFormat:sizeof(storage)));if(storage.Format.wFormatTag==WAVE_FORMAT_EXTENSIBLE&&mt->cbFormat>=sizeof(storage))guidText(storage.SubFormat,waveSub);}
    auto& f=storage.Format;
    write(event,L"snapshot=%u pin=%p hr=0x%08lx candidate=%d major=%s subtype=%s formattype=%s format_bytes=%lu sample_bytes=%lu wave=%d tag=%u rate=%lu bits=%u channels=%u align=%u avg=%lu cbSize=%u valid_bits=%u wave_subtype=%s",id,pin,ULONG(hr),candidate,major,sub,fmt,mt->cbFormat,mt->lSampleSize,wave?1:0,f.wFormatTag,f.nSamplesPerSec,f.wBitsPerSample,f.nChannels,f.nBlockAlign,f.nAvgBytesPerSec,f.cbSize,storage.Samples.wValidBitsPerSample,waveSub);
}
static void dump(IUnknown* object,const wchar_t* reason) {
    if(observing)return;observing=true;
    IFilterGraph* graph=nullptr;auto hr=object->QueryInterface(IID_PPV_ARGS(&graph));unsigned id=++serial;
    write(L"GRAPH_BEGIN",L"snapshot=%u reason=%s object=%p graph=%p hr=0x%08lx",id,reason,object,graph,ULONG(hr));
    if(graph){IEnumFilters* filters=nullptr;hr=graph->EnumFilters(&filters);
        if(filters){IBaseFilter* filter=nullptr;unsigned filterCount=0;
            while(filterCount++<128 && filters->Next(1,&filter,nullptr)==S_OK){
                FILTER_INFO fi{};filter->QueryFilterInfo(&fi);CLSID clsid{};filter->GetClassID(&clsid);wchar_t cls[40],module[MAX_PATH];guidText(clsid,cls);moduleOf((*reinterpret_cast<void***>(filter))[10],module);
                write(L"FILTER",L"snapshot=%u filter=%p name=%s clsid=%s module=%s",id,filter,fi.achName,cls,module);if(fi.pGraph)fi.pGraph->Release();
                IFileSourceFilter* source=nullptr;if(SUCCEEDED(filter->QueryInterface(IID_PPV_ARGS(&source)))){LPOLESTR path=nullptr;AM_MEDIA_TYPE mt{};auto result=source->GetCurFile(&path,&mt);write(L"SOURCE",L"snapshot=%u filter=%p hr=0x%08lx path=%s",id,filter,ULONG(result),path?path:L"");CoTaskMemFree(path);freeType(mt);source->Release();}
                IEnumPins* pins=nullptr;if(SUCCEEDED(filter->EnumPins(&pins))){IPin* pin=nullptr;unsigned pinCount=0;
                    while(pinCount++<128 && pins->Next(1,&pin,nullptr)==S_OK){
                        PIN_INFO info{};pin->QueryPinInfo(&info);IPin* peer=nullptr;auto connected=pin->ConnectedTo(&peer);wchar_t pinModule[MAX_PATH];moduleOf((*reinterpret_cast<void***>(pin))[7],pinModule);
                        write(L"PIN",L"snapshot=%u filter=%p pin=%p name=%s direction=%s peer=%p connected_hr=0x%08lx module=%s",id,filter,pin,info.achName,info.dir==PINDIR_INPUT?L"input":L"output",peer,ULONG(connected),pinModule);if(info.pFilter)info.pFilter->Release();if(peer)peer->Release();
                        AM_MEDIA_TYPE mt{};auto result=pin->ConnectionMediaType(&mt);media(id,L"CONNECTED_TYPE",pin,result,SUCCEEDED(result)?&mt:nullptr);freeType(mt);
                        IEnumMediaTypes* types=nullptr;result=pin->EnumMediaTypes(&types);
                        if(types){AM_MEDIA_TYPE* candidate=nullptr;unsigned count=0;while(count<24 && types->Next(1,&candidate,nullptr)==S_OK){media(id,L"ADVERTISED_TYPE",pin,S_OK,candidate,int(count++));freeType(*candidate);CoTaskMemFree(candidate);}write(L"ENUM_END",L"snapshot=%u pin=%p count=%u capped=%d",id,pin,count,count==24?1:0);types->Release();}
                        else write(L"ENUM_UNAVAILABLE",L"snapshot=%u pin=%p hr=0x%08lx",id,pin,ULONG(result));
                        pin->Release();
                    }pins->Release();
                }filter->Release();
            }filters->Release();
        }else write(L"ENUM_FILTERS_FAILED",L"snapshot=%u hr=0x%08lx",id,ULONG(hr));graph->Release();
    }write(L"GRAPH_END",L"snapshot=%u",id);observing=false;
}
using Run=HRESULT (STDMETHODCALLTYPE*)(IMediaControl*);
using FilterRun=HRESULT (STDMETHODCALLTYPE*)(IMediaFilter*,REFERENCE_TIME);
using RenderFile=HRESULT (STDMETHODCALLTYPE*)(IGraphBuilder*,LPCWSTR,LPCWSTR);
using Connect=HRESULT (STDMETHODCALLTYPE*)(IGraphBuilder*,IPin*,IPin*);
using Direct=HRESULT (STDMETHODCALLTYPE*)(IFilterGraph*,IPin*,IPin*,const AM_MEDIA_TYPE*);
static Run runOriginal;static FilterRun filterRunOriginal;static RenderFile renderOriginal;static Connect connectOriginal;static Direct directOriginal;
#ifdef ENABLE_DSEE_BYPASS
static const CLSID dseeId={0xA481D160,0xAB4D,0x4B97,{0x93,0xFE,0xC1,0xF4,0xB4,0xB7,0x81,0xDF}};
static const CLSID flacId={0x028A95AA,0xC956,0x4874,{0xAD,0x10,0xC6,0x57,0xDB,0x1B,0x00,0xF4}};
struct Edge {IPin* out=nullptr;IPin* in=nullptr;AM_MEDIA_TYPE mt{};};
static IPin* uniquePin(IBaseFilter* filter,PIN_DIRECTION direction){IEnumPins* e=nullptr;if(FAILED(filter->EnumPins(&e)))return nullptr;IPin* found=nullptr;IPin* p=nullptr;bool many=false;while(e->Next(1,&p,nullptr)==S_OK){PIN_DIRECTION d;p->QueryDirection(&d);if(d==direction){if(found){many=true;p->Release();break;}found=p;}else p->Release();}e->Release();if(many){found->Release();return nullptr;}return found;}
#ifdef ENABLE_TIMING_BRANCH
static IPin* freeOutput(IBaseFilter* filter){IEnumPins* e=nullptr;if(FAILED(filter->EnumPins(&e)))return nullptr;IPin* p=nullptr;while(e->Next(1,&p,nullptr)==S_OK){PIN_DIRECTION direction;p->QueryDirection(&direction);IPin* peer=nullptr;auto hr=p->ConnectedTo(&peer);if(peer)peer->Release();if(direction==PINDIR_OUTPUT&&FAILED(hr)){e->Release();return p;}p->Release();}e->Release();return nullptr;}
static void timingBranch(IGraphBuilder* graph,IMediaControl* control,std::vector<Edge>& edges){
    IEnumFilters* enumeration=nullptr;bool exists=false;if(SUCCEEDED(graph->EnumFilters(&enumeration))){IBaseFilter* f=nullptr;while(enumeration->Next(1,&f,nullptr)==S_OK){FILTER_INFO info{};f->QueryFilterInfo(&info);if(info.pGraph)info.pGraph->Release();if(wcscmp(info.achName,L"Probe timing branch tee")==0)exists=true;f->Release();}enumeration->Release();}if(exists){write(L"TIMING_BRANCH_ALREADY_PRESENT");return;}
    if(edges.size()!=4){write(L"TIMING_BRANCH_SKIP",L"unexpected_edges=%zu",edges.size());return;}
    CLSID expected[3];CLSIDFromString(L"{6AF73F1F-1266-46E6-9A36-EE75E4BA10BB}",&expected[0]);CLSIDFromString(L"{5C968488-5E35-4CFE-98A1-A51C838797DB}",&expected[1]);expected[2]=CLSID_DSoundRender;
    for(size_t i=1;i<edges.size();++i){PIN_INFO pi{};if(FAILED(edges[i].in->QueryPinInfo(&pi))||!pi.pFilter){write(L"TIMING_BRANCH_SKIP",L"missing_filter_at=%zu",i);return;}CLSID actual{};auto r=pi.pFilter->GetClassID(&actual);pi.pFilter->Release();if(FAILED(r)||actual!=expected[i-1]){write(L"TIMING_BRANCH_SKIP",L"unexpected_filter_at=%zu",i);return;}}
    // Retain the original source, DSEE, Seamless, EQ and renderer objects. Only the first and last edges change.
    CLSID nullRenderer;CLSIDFromString(L"{C1F400A4-3F08-11D3-9F0B-006008039E37}",&nullRenderer);
    IBaseFilter* tee=nullptr;IBaseFilter* sink=nullptr;auto hr=CoCreateInstance(CLSID_InfTee,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&tee));if(SUCCEEDED(hr))hr=CoCreateInstance(nullRenderer,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&sink));
    bool addedTee=false,addedSink=false,disconnected=false;IPin* teeIn=nullptr;IPin* teeMain=nullptr;IPin* teeClock=nullptr;IPin* sinkIn=nullptr;
    IMediaSeeking* seeking=nullptr;LONGLONG position=0;graph->QueryInterface(IID_PPV_ARGS(&seeking));bool havePosition=seeking&&SUCCEEDED(seeking->GetCurrentPosition(&position));
    if(SUCCEEDED(hr)){hr=control->Stop();if(SUCCEEDED(hr)){graph->Disconnect(edges.front().out);graph->Disconnect(edges.front().in);graph->Disconnect(edges.back().out);graph->Disconnect(edges.back().in);disconnected=true;hr=graph->AddFilter(tee,L"Probe timing branch tee");addedTee=SUCCEEDED(hr);}}
    if(SUCCEEDED(hr)){hr=graph->AddFilter(sink,L"Probe silent timing sink");addedSink=SUCCEEDED(hr);}
    if(SUCCEEDED(hr)){teeIn=uniquePin(tee,PINDIR_INPUT);sinkIn=uniquePin(sink,PINDIR_INPUT);if(!teeIn||!sinkIn)hr=E_NOINTERFACE;}
    if(SUCCEEDED(hr)){hr=graph->ConnectDirect(edges.front().out,teeIn,&edges.front().mt);write(L"TIMING_CONNECT_SOURCE_TEE",L"hr=%08lx",ULONG(hr));}
    if(SUCCEEDED(hr)){teeClock=freeOutput(tee);if(!teeClock)hr=E_NOINTERFACE;else hr=graph->ConnectDirect(teeClock,edges.front().in,&edges.front().mt);write(L"TIMING_CONNECT_SONY_BRANCH",L"hr=%08lx",ULONG(hr));}
    if(SUCCEEDED(hr)){hr=graph->ConnectDirect(edges.back().out,sinkIn,&edges.back().mt);write(L"TIMING_CONNECT_SILENT_SINK",L"hr=%08lx",ULONG(hr));}
    if(SUCCEEDED(hr)){teeMain=freeOutput(tee);if(!teeMain)hr=E_NOINTERFACE;else hr=graph->ConnectDirect(teeMain,edges.back().in,&edges.front().mt);write(L"TIMING_CONNECT_AUDIO_BRANCH",L"hr=%08lx",ULONG(hr));}
    if(FAILED(hr)){write(L"TIMING_ROLLBACK_BEGIN",L"failure=%08lx",ULONG(hr));if(teeMain){graph->Disconnect(teeMain);graph->Disconnect(edges.back().in);}if(teeClock){graph->Disconnect(teeClock);graph->Disconnect(edges.front().in);}if(teeIn){graph->Disconnect(teeIn);graph->Disconnect(edges.front().out);}if(sinkIn){graph->Disconnect(sinkIn);graph->Disconnect(edges.back().out);}if(addedSink)graph->RemoveFilter(sink);if(addedTee)graph->RemoveFilter(tee);HRESULT restored=S_OK;if(disconnected){auto a=graph->ConnectDirect(edges.front().out,edges.front().in,&edges.front().mt);auto b=graph->ConnectDirect(edges.back().out,edges.back().in,&edges.back().mt);restored=FAILED(a)?a:b;}write(L"TIMING_ROLLBACK_END",L"hr=%08lx",ULONG(restored));}
    else write(L"TIMING_BRANCH_APPLIED",L"graph=%p sony_filters_preserved=1",graph);
    if(havePosition){auto r=seeking->SetPositions(&position,AM_SEEKING_AbsolutePositioning,nullptr,AM_SEEKING_NoPositioning);write(L"TIMING_RESTORE_POSITION",L"position=%lld hr=%08lx",position,ULONG(r));}if(seeking)seeking->Release();
    if(teeIn)teeIn->Release();if(teeMain)teeMain->Release();if(teeClock)teeClock->Release();if(sinkIn)sinkIn->Release();if(tee)tee->Release();if(sink)sink->Release();
}
#endif
static void bypass(IMediaControl* control){
    IGraphBuilder* graph=nullptr;if(FAILED(control->QueryInterface(IID_PPV_ARGS(&graph))))return;
    IEnumFilters* enumeration=nullptr;IBaseFilter* dsee=nullptr;graph->EnumFilters(&enumeration);
    if(enumeration){IBaseFilter* f=nullptr;while(enumeration->Next(1,&f,nullptr)==S_OK){CLSID id{};f->GetClassID(&id);if(id==dseeId){dsee=f;break;}f->Release();}enumeration->Release();}
    if(!dsee){write(L"BYPASS_NOT_PRESENT");graph->Release();return;}
    IPin* input=uniquePin(dsee,PINDIR_INPUT);IPin* output=uniquePin(dsee,PINDIR_OUTPUT);IPin* source=nullptr;
    if(input)input->ConnectedTo(&source);bool eligible=false;
    if(source){PIN_INFO pi{};source->QueryPinInfo(&pi);if(pi.pFilter){CLSID id{};pi.pFilter->GetClassID(&id);eligible=id==flacId;pi.pFilter->Release();}}
    std::vector<Edge> edges;
    if(eligible&&input&&output){source->AddRef();input->AddRef();edges.push_back({source,input,{}});IPin* next=output;next->AddRef();
        for(unsigned count=0;next&&count<8;++count){IPin* target=nullptr;auto hr=next->ConnectedTo(&target);if(FAILED(hr)){next->Release();next=nullptr;eligible=false;break;}edges.push_back({next,target,{}});PIN_INFO info{};target->QueryPinInfo(&info);next=info.pFilter?uniquePin(info.pFilter,PINDIR_OUTPUT):nullptr;if(info.pFilter)info.pFilter->Release();}
        if(next){next->Release();eligible=false;}
        for(auto& e:edges){if(FAILED(e.out->ConnectionMediaType(&e.mt))||e.mt.majortype!=MEDIATYPE_Audio)eligible=false;}
    }
    FILTER_INFO filterInfo{};dsee->QueryFilterInfo(&filterInfo);if(filterInfo.pGraph)filterInfo.pGraph->Release();
    if(eligible&&edges.size()>=2){
#ifdef ENABLE_TIMING_BRANCH
        timingBranch(graph,control,edges);
#else
        OAFilterState state;auto stateHr=control->GetState(1000,&state);write(L"BYPASS_PLAN",L"edges=%zu state_hr=%08lx state=%ld",edges.size(),ULONG(stateHr),long(state));
        IMediaSeeking* seeking=nullptr;LONGLONG position=0;graph->QueryInterface(IID_PPV_ARGS(&seeking));bool havePosition=seeking&&SUCCEEDED(seeking->GetCurrentPosition(&position));
        auto hr=control->Stop();bool removed=false;
        if(SUCCEEDED(hr)){for(auto& e:edges){graph->Disconnect(e.out);graph->Disconnect(e.in);}hr=graph->RemoveFilter(dsee);removed=SUCCEEDED(hr);}
        std::vector<IBaseFilter*> extraRemoved;
        std::vector<std::wstring> extraNames;
        if(SUCCEEDED(hr)){hr=graph->ConnectDirect(edges[0].out,edges[1].in,&edges[0].mt);write(L"BYPASS_CONNECT",L"edge=source_to_downstream hr=%08lx",ULONG(hr));for(size_t i=2;SUCCEEDED(hr)&&i<edges.size();++i){hr=graph->ConnectDirect(edges[i].out,edges[i].in,nullptr);write(L"BYPASS_CONNECT",L"edge=%zu hr=%08lx",i,ULONG(hr));}}
        if(FAILED(hr)&&removed){write(L"BYPASS_DIRECT_FALLBACK",L"prior_hr=%08lx",ULONG(hr));for(auto& e:edges){graph->Disconnect(e.out);graph->Disconnect(e.in);}hr=S_OK;
            for(size_t i=1;i+1<edges.size();++i){PIN_INFO pi{};edges[i].in->QueryPinInfo(&pi);if(pi.pFilter){FILTER_INFO fi{};pi.pFilter->QueryFilterInfo(&fi);if(fi.pGraph)fi.pGraph->Release();CLSID cls{};pi.pFilter->GetClassID(&cls);CLSID seamless,eq;CLSIDFromString(L"{6AF73F1F-1266-46E6-9A36-EE75E4BA10BB}",&seamless);CLSIDFromString(L"{5C968488-5E35-4CFE-98A1-A51C838797DB}",&eq);if(cls!=seamless&&cls!=eq){pi.pFilter->Release();hr=E_NOINTERFACE;break;}auto r=graph->RemoveFilter(pi.pFilter);if(FAILED(r)){pi.pFilter->Release();hr=r;break;}extraRemoved.push_back(pi.pFilter);extraNames.push_back(fi.achName);}else {hr=E_NOINTERFACE;break;}}
            if(SUCCEEDED(hr)){hr=graph->ConnectDirect(edges[0].out,edges.back().in,&edges[0].mt);write(L"BYPASS_DIRECT_RESULT",L"hr=%08lx",ULONG(hr));}
        }
        if(FAILED(hr)){write(L"BYPASS_ROLLBACK_BEGIN",L"failure=%08lx",ULONG(hr));for(auto& e:edges){graph->Disconnect(e.out);graph->Disconnect(e.in);}HRESULT rollback=S_OK;if(removed)rollback=graph->AddFilter(dsee,filterInfo.achName);for(size_t i=0;i<extraRemoved.size();++i){auto r=graph->AddFilter(extraRemoved[i],extraNames[i].c_str());if(FAILED(r))rollback=r;}for(auto& e:edges){auto r=graph->ConnectDirect(e.out,e.in,&e.mt);if(FAILED(r))rollback=r;}write(L"BYPASS_ROLLBACK_END",L"hr=%08lx",ULONG(rollback));}
        else write(L"BYPASS_APPLIED",L"graph=%p removed_filter=%s extra_removed=%zu",graph,filterInfo.achName,extraRemoved.size());
        for(auto f:extraRemoved)f->Release();
        if(havePosition){auto result=seeking->SetPositions(&position,AM_SEEKING_AbsolutePositioning,nullptr,AM_SEEKING_NoPositioning);write(L"BYPASS_RESTORE_POSITION",L"position=%lld hr=%08lx",position,ULONG(result));}if(seeking)seeking->Release();
#endif
    }else write(L"BYPASS_SKIPPED",L"eligible=%d edges=%zu",eligible?1:0,edges.size());
    for(auto& e:edges){freeType(e.mt);e.out->Release();e.in->Release();}if(source)source->Release();if(input)input->Release();if(output)output->Release();dsee->Release();graph->Release();
}
#endif
static HRESULT STDMETHODCALLTYPE runHook(IMediaControl* self){
#ifdef ENABLE_DSEE_BYPASS
    bypass(self);
#endif
    dump(self,L"IMediaControl.Run.before");auto hr=runOriginal(self);write(L"RUN_RESULT",L"object=%p hr=0x%08lx",self,ULONG(hr));return hr;}
static HRESULT STDMETHODCALLTYPE filterRunHook(IMediaFilter* self,REFERENCE_TIME time){dump(self,L"IMediaFilter.Run.before");return filterRunOriginal(self,time);}
static HRESULT STDMETHODCALLTYPE renderHook(IGraphBuilder* self,LPCWSTR path,LPCWSTR list){write(L"RENDER_FILE",L"graph=%p path=%s",self,path?path:L"");auto hr=renderOriginal(self,path,list);dump(self,L"RenderFile.after");return hr;}
static HRESULT STDMETHODCALLTYPE connectHook(IGraphBuilder* self,IPin* out,IPin* in){auto hr=connectOriginal(self,out,in);write(L"CONNECT",L"graph=%p out=%p in=%p hr=0x%08lx",self,out,in,ULONG(hr));dump(self,L"Connect.after");return hr;}
static HRESULT STDMETHODCALLTYPE directHook(IFilterGraph* self,IPin* out,IPin* in,const AM_MEDIA_TYPE* mt){media(0,L"CONNECT_DIRECT_REQUEST",out,S_OK,mt);auto hr=directOriginal(self,out,in,mt);write(L"CONNECT_DIRECT",L"graph=%p out=%p in=%p hr=0x%08lx",self,out,in,ULONG(hr));dump(self,L"ConnectDirect.after");return hr;}
template<class T> static bool install(void* object,int slot,void* detour,T* original,const wchar_t* name){void* address=(*reinterpret_cast<void***>(object))[slot];HMODULE owner=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(address),&owner);auto status=MH_CreateHook(address,detour,reinterpret_cast<void**>(original));write(L"HOOK",L"name=%s address=%p status=%d",name,address,int(status));return status==MH_OK;}
static DWORD WINAPI start(void*){
    wchar_t temp[MAX_PATH],path[MAX_PATH];GetTempPathW(MAX_PATH,temp);
#if defined(ENABLE_TIMING_BRANCH)
    swprintf_s(path,L"%sDirectShowTimingBypass-%lu.tsv",temp,GetCurrentProcessId());
#elif defined(ENABLE_DSEE_BYPASS)
    swprintf_s(path,L"%sDirectShowBypass-%lu.tsv",temp,GetCurrentProcessId());
#else
    swprintf_s(path,L"%sDirectShowProbe-%lu.tsv",temp,GetCurrentProcessId());
#endif
    file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return 1;
#ifdef ENABLE_DSEE_BYPASS
    write(L"START",L"arch=%u observation_only=0 bypass_dsee_for_sony_flac=1",unsigned(sizeof(void*)*8));
#else
    write(L"START",L"arch=%u observation_only=1",unsigned(sizeof(void*)*8));
#endif
    auto co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);IGraphBuilder* graph=nullptr;auto hr=CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&graph));write(L"SETUP_GRAPH",L"hr=0x%08lx",ULONG(hr));
    bool ok=graph && MH_Initialize()==MH_OK;
    if(ok){ok=install(graph,13,reinterpret_cast<void*>(renderHook),&renderOriginal,L"RenderFile")&&ok;ok=install(graph,11,reinterpret_cast<void*>(connectHook),&connectOriginal,L"Connect")&&ok;ok=install(graph,7,reinterpret_cast<void*>(directHook),&directOriginal,L"ConnectDirect")&&ok;
        IMediaControl* control=nullptr;if(SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&control)))){ok=install(control,7,reinterpret_cast<void*>(runHook),&runOriginal,L"IMediaControl.Run")&&ok;control->Release();}else ok=false;
        IMediaFilter* mf=nullptr;if(SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&mf)))){ok=install(mf,6,reinterpret_cast<void*>(filterRunHook),&filterRunOriginal,L"IMediaFilter.Run")&&ok;mf->Release();}
        auto status=MH_EnableHook(MH_ALL_HOOKS);write(L"ENABLE",L"status=%d",int(status));ok=ok&&status==MH_OK;
    }
    if(graph)graph->Release();if(SUCCEEDED(co))CoUninitialize();write(ok?L"READY":L"FAILED");return ok?0:2;
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){DisableThreadLibraryCalls(module);HANDLE thread=CreateThread(nullptr,0,start,nullptr,0,nullptr);if(!thread)return FALSE;CloseHandle(thread);}return TRUE;}
