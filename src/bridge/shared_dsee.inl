// Observed x86 interface slot 3 takes nine DWORDs by value.
struct SharedDseeConfig {
    DWORD mode,channels,format,inputRate,outputRate,inputBits,outputBits,unknown,quality;
};
static_assert(sizeof(SharedDseeConfig)==36);
using Microsoft::WRL::ComPtr;
using DseeSetter=HRESULT(STDMETHODCALLTYPE*)(IUnknown*,SharedDseeConfig);
using FilterPause=HRESULT(STDMETHODCALLTYPE*)(IMediaFilter*);
static DseeSetter originalDsee=nullptr;
static Run originalPause=nullptr;
static FilterPause originalFilterPause=nullptr;
static thread_local bool preparingDsee=false;
struct DseeScope {
    bool outer=!preparingDsee;
    DseeScope(){preparingDsee=true;}
    ~DseeScope(){if(outer)preparingDsee=false;}
};
static const CLSID dsee={0xa481d160,0xab4d,0x4b97,{0x93,0xfe,0xc1,0xf4,0xb4,0xb7,0x81,0xdf}};
static const CLSID seamless={0x6af73f1f,0x1266,0x46e6,{0x9a,0x36,0xee,0x75,0xe4,0xba,0x10,0xbb}};
static SRWLOCK dseeLock=SRWLOCK_INIT;
static std::map<IBaseFilter*,bool> dseeConfigured;

static bool configuredDsee(IBaseFilter* filter) {
    AcquireSRWLockShared(&dseeLock);
    const bool found=dseeConfigured.count(filter)!=0;
    ReleaseSRWLockShared(&dseeLock);
    return found;
}
static bool eligibleDsee(const SharedDseeConfig& c) {
    if(c.mode!=1 || c.channels!=2 || c.inputRate!=44100 || c.inputBits!=16 ||
       c.outputRate!=44100 || c.outputBits!=16)return false;
    if(c.unknown==1)return (c.format==3 && c.quality==96) || (c.format==101 && c.quality==0);
    return c.unknown==0 && ((c.format==2 && c.quality==96) || (c.format==16 && c.quality==0));
}
static HRESULT STDMETHODCALLTYPE sharedDseeSetter(IUnknown* object,SharedDseeConfig c) {
    ComPtr<IBaseFilter> filter;
    if(FAILED(object->QueryInterface(IID_PPV_ARGS(&filter))))return originalDsee(object,c);

    AcquireSRWLockExclusive(&dseeLock);
    dseeConfigured.erase(filter.Get());
    ReleaseSRWLockExclusive(&dseeLock);
    if(!eligibleDsee(c) || !bridgeStatus::dseeSharedRequested())return originalDsee(object,c);

    auto adjusted=c;
    adjusted.outputRate=c.format==3?96000:176400;
    adjusted.outputBits=24;
    const auto hr=originalDsee(object,adjusted);
    record("DSEE_SET",c.format,adjusted.outputRate,hr);
    if(FAILED(hr))return originalDsee(object,c);
    AcquireSRWLockExclusive(&dseeLock);
    if(dseeConfigured.size()>=64)dseeConfigured.erase(dseeConfigured.begin());
    dseeConfigured.emplace(filter.Get(),true);
    ReleaseSRWLockExclusive(&dseeLock);
    return hr;
}
static bool ownedBy(IPin* pin,REFCLSID cls) {
    PIN_INFO info{};
    if(FAILED(pin->QueryPinInfo(&info)) || !info.pFilter)return false;
    CLSID id{};
    const auto hr=info.pFilter->GetClassID(&id);
    info.pFilter->Release();
    return SUCCEEDED(hr) && id==cls;
}
#include "shared_dsee_route.inl"

static HRESULT STDMETHODCALLTYPE pauseDsee(IMediaControl* self) {
    DseeScope scope;
    const auto hr=scope.outer?prepareDirect(self):S_OK;
    return FAILED(hr)?hr:originalPause(self);
}
static HRESULT STDMETHODCALLTYPE filterPauseDsee(IMediaFilter* self) {
    DseeScope scope;
    const auto hr=scope.outer?prepareDirect(self):S_OK;
    return FAILED(hr)?hr:originalFilterPause(self);
}
static bool setupDseePause(IMediaControl* control,IMediaFilter* filter) {
    auto controlPause=(*reinterpret_cast<void***>(control))[8];
    auto filterPause=(*reinterpret_cast<void***>(filter))[5];
    return MH_CreateHook(controlPause,reinterpret_cast<void*>(pauseDsee),reinterpret_cast<void**>(&originalPause))==MH_OK &&
           MH_CreateHook(filterPause,reinterpret_cast<void*>(filterPauseDsee),reinterpret_cast<void**>(&originalFilterPause))==MH_OK;
}
static bool setupSharedDsee() {
    auto module=LoadLibraryExW(LR"(C:\Program Files (x86)\Sony\Music Center\Sony.Earth\OmgDseeHxFilter.ax)",
                              nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if(!module)return false;
    auto get=reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID,REFIID,void**)>(GetProcAddress(module,"DllGetClassObject"));
    ComPtr<IClassFactory> factory;
    ComPtr<IBaseFilter> filter;
    ComPtr<IUnknown> control;
    IID iid{};
    CLSIDFromString(L"{6AF76DBE-8634-4FD3-93ED-982EDE1BFDD5}",&iid);
    if(!get || FAILED(get(dsee,IID_PPV_ARGS(&factory))) ||
       FAILED(factory->CreateInstance(nullptr,IID_PPV_ARGS(&filter))) || FAILED(filter->QueryInterface(iid,reinterpret_cast<void**>(control.GetAddressOf()))))return false;

    auto base=reinterpret_cast<BYTE*>(module);
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    auto fn=(*reinterpret_cast<void***>(control.Get()))[3];
    return nt->FileHeader.TimeDateStamp==0x63eb12c1 && reinterpret_cast<BYTE*>(fn)-base==0x36ca &&
           MH_CreateHook(fn,reinterpret_cast<void*>(sharedDseeSetter),reinterpret_cast<void**>(&originalDsee))==MH_OK;
}
