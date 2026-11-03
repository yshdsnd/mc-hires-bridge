// Included only by the WAV experiment, after the shared FLAC hook state.
static thread_local bool preparingWav=false;
using DseeSetter=HRESULT(STDMETHODCALLTYPE*)(IUnknown*,wav192::DseeConfig);
static DseeSetter originalDsee=nullptr;
static Run originalPause=nullptr;
static HRESULT(STDMETHODCALLTYPE* originalFilterPause)(IMediaFilter*)=nullptr;
static bool knownWavCaller(void* returnAddress){
    HMODULE m=nullptr;if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(returnAddress),&m))return false;
    wchar_t path[MAX_PATH]{};GetModuleFileNameW(m,path,MAX_PATH);auto name=wcsrchr(path,L'\\');auto base=reinterpret_cast<BYTE*>(m);auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(m);auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    return _wcsicmp(name?name+1:path,L"OpcWavEx.dll")==0&&nt->FileHeader.TimeDateStamp==0x63eb12e7&&reinterpret_cast<BYTE*>(returnAddress)-base==0x33e47;
}
static HRESULT STDMETHODCALLTYPE dseeSetterHook(IUnknown* self,wav192::DseeConfig config){
    record("DSEE_REQUEST_INPUT",config.inputRate,config.inputBits,knownWavCaller(_ReturnAddress())?S_OK:S_FALSE);
    record("DSEE_REQUEST_OUTPUT",config.outputRate,config.outputBits,S_OK);
    record("DSEE_REQUEST_MODE_FORMAT",config.mode,config.format,HRESULT(config.channels));
    wav192::DseeConfig adjusted{};
    if(!knownWavCaller(_ReturnAddress())||!bridgeStatus::directSoundSelected()||!wav192::nativeConfig(config,adjusted)){
        bool previouslyChanged=false;Microsoft::WRL::ComPtr<IBaseFilter> filter;
        if(SUCCEEDED(self->QueryInterface(IID_PPV_ARGS(&filter)))){AcquireSRWLockShared(&sourcesLock);auto it=appliedSources.find(filter.Get());previouslyChanged=it!=appliedSources.end()&&it->second;ReleaseSRWLockShared(&sourcesLock);}
        auto hr=originalDsee(self,config);
        bool keepsNative=config.channels==2&&config.inputRate==192000&&config.outputRate==192000&&config.inputBits==24&&config.outputBits==24&&config.format==24;
        rememberSource(self,previouslyChanged&&keepsNative&&SUCCEEDED(hr));return hr;
    }
    auto hr=originalDsee(self,adjusted);rememberSource(self,SUCCEEDED(hr));record("WAV192_DSEE_NATIVE",config.outputRate,adjusted.outputRate,hr);
    if(FAILED(hr)){auto fallback=originalDsee(self,config);record("WAV192_DSEE_FALLBACK",config.outputRate,config.outputRate,fallback);return fallback;}
    return hr;
}
static bool setupDseeHook(){
    auto module=LoadLibraryExW(L"C:\\Program Files (x86)\\Sony\\Music Center\\Sony.Earth\\OmgDseeHxFilter.ax",nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if(!module)return false;auto get=reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID,REFIID,void**)>(GetProcAddress(module,"DllGetClassObject"));if(!get)return false;
    Microsoft::WRL::ComPtr<IClassFactory> factory;Microsoft::WRL::ComPtr<IUnknown> control;IID iid;CLSIDFromString(L"{6AF76DBE-8634-4FD3-93ED-982EDE1BFDD5}",&iid);
    if(FAILED(get(wav192::dseeId,IID_PPV_ARGS(&factory)))||FAILED(factory->CreateInstance(nullptr,iid,reinterpret_cast<void**>(control.GetAddressOf()))))return false;
    auto base=reinterpret_cast<BYTE*>(module);auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);auto fn=(*reinterpret_cast<void***>(control.Get()))[3];
    if(nt->FileHeader.TimeDateStamp!=0x63eb12c1||reinterpret_cast<BYTE*>(fn)-base!=0x36ca)return false;
    auto status=MH_CreateHook(fn,reinterpret_cast<void*>(dseeSetterHook),reinterpret_cast<void**>(&originalDsee));record("CREATE_DSEE_HOOK",0,0,HRESULT(status));
    if(status==MH_OK){HMODULE pinned=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(fn),&pinned);}return status==MH_OK;
}
static bool configuredDseeStillConnected(IGraphBuilder* graph){
    Microsoft::WRL::ComPtr<IEnumFilters> filters;if(FAILED(graph->EnumFilters(&filters)))return true;
    Microsoft::WRL::ComPtr<IBaseFilter> f;while(filters->Next(1,&f,nullptr)==S_OK){CLSID cls{};
        if(SUCCEEDED(f->GetClassID(&cls))&&cls==wav192::dseeId){AcquireSRWLockShared(&sourcesLock);auto it=appliedSources.find(f.Get());bool changed=it!=appliedSources.end()&&it->second;ReleaseSRWLockShared(&sourcesLock);
            if(changed){for(auto direction:{PINDIR_INPUT,PINDIR_OUTPUT}){auto pin=wav192::uniquePin(f.Get(),direction);Microsoft::WRL::ComPtr<IPin> peer;if(!pin||SUCCEEDED(pin->ConnectedTo(&peer)))return true;}}}
        f.Reset();}return false;
}
static HRESULT prepareWav(IUnknown* object){
    if(!bridgeStatus::directSoundSelected())return S_OK;
    Microsoft::WRL::ComPtr<IGraphBuilder> graph;
    if(FAILED(object->QueryInterface(IID_PPV_ARGS(&graph))))return S_OK;
    Microsoft::WRL::ComPtr<IEnumFilters> diagnostic;
    if(SUCCEEDED(graph->EnumFilters(&diagnostic))){Microsoft::WRL::ComPtr<IBaseFilter> filter;while(diagnostic->Next(1,&filter,nullptr)==S_OK){CLSID cls{};filter->GetClassID(&cls);wchar_t id[40];StringFromGUID2(cls,id,40);char line[512];auto n=sprintf_s(line,"GRAPH_FILTER\tcls=%ls\r\n",id);AcquireSRWLockExclusive(&mutex);DWORD written;if(n>0)WriteFile(output,line,n,&written,nullptr);ReleaseSRWLockExclusive(&mutex);Microsoft::WRL::ComPtr<IEnumPins> pins;if(SUCCEEDED(filter->EnumPins(&pins))){Microsoft::WRL::ComPtr<IPin> pin;while(pins->Next(1,&pin,nullptr)==S_OK){AM_MEDIA_TYPE mt{};PIN_DIRECTION d{};pin->QueryDirection(&d);if(SUCCEEDED(pin->ConnectionMediaType(&mt))&&mt.formattype==FORMAT_WaveFormatEx&&mt.pbFormat&&mt.cbFormat>=16){WAVEFORMATEX wave{};memcpy(&wave,mt.pbFormat,min(size_t(mt.cbFormat),sizeof(wave)));record(d==PINDIR_INPUT?"GRAPH_INPUT":"GRAPH_OUTPUT",wave.nSamplesPerSec,wave.wBitsPerSample,HRESULT(wave.nChannels));}if(mt.cbFormat)CoTaskMemFree(mt.pbFormat);if(mt.pUnk)mt.pUnk->Release();pin.Reset();}}filter.Reset();}}
    auto result=wav192::apply(graph.Get());
    const char* event=result.outcome==wav192::Outcome::Applied?"WAV192_APPLIED":result.outcome==wav192::Outcome::AlreadyNative?"WAV192_NATIVE":result.outcome==wav192::Outcome::Restored?"WAV192_RESTORED":result.outcome==wav192::Outcome::RestoreFailed?"WAV192_RESTORE_FAILED":"WAV192_SKIPPED";
    record(event,DWORD(result.outcome),DWORD(result.restore),result.change);
    // A native-configured DSEE must be bypassed; its independent tail test lost samples.
    if(configuredDseeStillConnected(graph.Get())){record("WAV192_BYPASS_REQUIRED",0,0,E_FAIL);bridgeStatus::state.store(bridgeStatus::State::Off);return E_FAIL;}
    if(result.outcome==wav192::Outcome::Applied||result.outcome==wav192::Outcome::AlreadyNative){
        Microsoft::WRL::ComPtr<IEnumFilters> filters;
        if(SUCCEEDED(graph->EnumFilters(&filters))){Microsoft::WRL::ComPtr<IBaseFilter> f;while(filters->Next(1,&f,nullptr)==S_OK){CLSID cls{};if(SUCCEEDED(f->GetClassID(&cls))&&wav192::isParser(cls))rememberSource(f.Get(),true);f.Reset();}}
    }
    if(result.outcome==wav192::Outcome::RestoreFailed){bridgeStatus::state.store(bridgeStatus::State::Off);return E_FAIL;}
    return S_OK;
}
struct WavScope {bool outer;WavScope():outer(!preparingWav){preparingWav=true;}~WavScope(){if(outer)preparingWav=false;}};
static HRESULT STDMETHODCALLTYPE pauseHook(IMediaControl* self){WavScope scope;if(scope.outer){auto hr=prepareWav(self);if(FAILED(hr))return hr;}return originalPause(self);}
static HRESULT STDMETHODCALLTYPE filterPauseHook(IMediaFilter* self){WavScope scope;if(scope.outer){auto hr=prepareWav(self);if(FAILED(hr))return hr;}return originalFilterPause(self);}
