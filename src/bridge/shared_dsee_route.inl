struct ConnectedEdge {
    ComPtr<IPin> out, in;
    AM_MEDIA_TYPE type{};
    ~ConnectedEdge() {
        CoTaskMemFree(type.pbFormat);
        if(type.pUnk)type.pUnk->Release();
    }
    bool read() {
        return out && SUCCEEDED(out->ConnectedTo(&in)) && SUCCEEDED(out->ConnectionMediaType(&type));
    }
};

static ComPtr<IPin> uniqueDseePin(IBaseFilter* filter,PIN_DIRECTION expected) {
    ComPtr<IEnumPins> pins;
    ComPtr<IPin> found, pin;
    if(FAILED(filter->EnumPins(&pins)))return {};
    while(pins->Next(1,pin.ReleaseAndGetAddressOf(),nullptr)==S_OK) {
        PIN_DIRECTION direction{};
        if(SUCCEEDED(pin->QueryDirection(&direction)) && direction==expected) {
            if(found)return {};
            found=pin;
        }
    }
    return found;
}
static ComPtr<IBaseFilter> pinFilter(IPin* pin) {
    PIN_INFO info{};
    ComPtr<IBaseFilter> result;
    if(SUCCEEDED(pin->QueryPinInfo(&info)))result.Attach(info.pFilter);
    return result;
}
static bool filterName(IBaseFilter* filter,const wchar_t* expected) {
    FILTER_INFO info{};
    if(!filter || FAILED(filter->QueryFilterInfo(&info)))return false;
    if(info.pGraph)info.pGraph->Release();
    return wcscmp(info.achName,expected)==0;
}

static bool dseePcm(const AM_MEDIA_TYPE& type) {
    if(type.formattype!=FORMAT_WaveFormatEx || type.cbFormat<16 || !type.pbFormat)return false;
    const auto& wave=*reinterpret_cast<const WAVEFORMATEX*>(type.pbFormat);
    return wave.nSamplesPerSec==176400 && wave.wBitsPerSample==24 &&
           wave.nChannels==2 && wave.wFormatTag==WAVE_FORMAT_PCM;
}

static bool dseeFeedsRenderer(IPin* rendererPin,const AM_MEDIA_TYPE& format) {
    if(!dseePcm(format))return false;
    ComPtr<IPin> peer;
    if(rendererPin->ConnectedTo(&peer)!=S_OK)return false;
    if(ownedBy(peer.Get(),seamless)) {
        auto filter=pinFilter(peer.Get());
        if(!filter)return false;
        auto input=uniqueDseePin(filter.Get(),PINDIR_INPUT);
        if(!input || input->ConnectedTo(peer.ReleaseAndGetAddressOf())!=S_OK)return false;
    }
    if(!ownedBy(peer.Get(),dsee))return false;
    auto filter=pinFilter(peer.Get());
    return filter && configuredDsee(filter.Get());
}

static HRESULT prepareDirect(IUnknown* self) {
    if(!bridgeStatus::directSoundSelected())return S_OK;
    ComPtr<IFilterGraph> graph;
    ComPtr<IMediaFilter> media;
    if(FAILED(self->QueryInterface(IID_PPV_ARGS(&graph))) ||
       FAILED(self->QueryInterface(IID_PPV_ARGS(&media))))return S_OK;
    FILTER_STATE state{};
    if(media->GetState(0,&state)!=S_OK || state!=State_Stopped)return S_OK;
    ComPtr<IEnumFilters> filters;
    if(FAILED(graph->EnumFilters(&filters)))return S_OK;
    ComPtr<IBaseFilter> filter;
    while(filters->Next(1,filter.ReleaseAndGetAddressOf(),nullptr)==S_OK) {
        CLSID id{};
        if(SUCCEEDED(filter->GetClassID(&id)) && id==dsee)break;
    }
    if(!filter || !configuredDsee(filter.Get()))return S_OK;
    auto output=uniqueDseePin(filter.Get(),PINDIR_OUTPUT);

    ComPtr<IPin> downstream;
    if(output && output->ConnectedTo(&downstream)==S_OK && ownedBy(downstream.Get(),seamless)) {
        auto seamlessFilter=pinFilter(downstream.Get());
        if(!seamlessFilter)return S_OK;
        output=uniqueDseePin(seamlessFilter.Get(),PINDIR_OUTPUT);
    }

    ConnectedEdge edges[3];
    edges[0].out=output;
    if(!edges[0].read())return S_OK;
    auto next=pinFilter(edges[0].in.Get());
    unsigned last=1;
    if(filterName(next.Get(),L"OmgFsConvFilter")) {
        edges[1].out=uniqueDseePin(next.Get(),PINDIR_OUTPUT);
        if(!edges[1].read())return S_OK;
        next=pinFilter(edges[1].in.Get());
        last=2;
    }
    if(!filterName(next.Get(),L"OmgEqxlFilter"))return S_OK;
    edges[last].out=uniqueDseePin(next.Get(),PINDIR_OUTPUT);
    if(!edges[last].read() || !ownedBy(edges[last].in.Get(),CLSID_DSoundRender))return S_OK;
    const auto& type=edges[0].type;
    if(!dseePcm(type))return S_OK;
    HRESULT hr=S_OK;
    for(unsigned i=0;i<=last;++i) {
        auto& edge=edges[i];
        hr=graph->Disconnect(edge.out.Get());
        if(FAILED(hr))break;
        hr=graph->Disconnect(edge.in.Get());
        if(FAILED(hr))break;
    }
    if(SUCCEEDED(hr))hr=graph->ConnectDirect(edges[0].out.Get(),edges[last].in.Get(),&type);
    record("DSEE_DIRECT",176400,24,hr);

    if(hr==S_OK)return S_OK;

    HRESULT restored=S_OK;
    for(unsigned i=0;i<=last;++i) {
        auto& edge=edges[i];
        ComPtr<IPin> connected;
        if(edge.out->ConnectedTo(&connected)==S_OK && connected.Get()==edge.in.Get())continue;
        graph->Disconnect(edge.out.Get());
        graph->Disconnect(edge.in.Get());
        const auto result=graph->ConnectDirect(edge.out.Get(),edge.in.Get(),&edge.type);
        if(FAILED(result) && SUCCEEDED(restored))restored=result;
    }
    record("DSEE_RESTORE",0,0,restored);

    return restored;
}
