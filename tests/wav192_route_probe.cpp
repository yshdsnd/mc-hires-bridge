#include "wav192_route.h"
#include <cstdio>
#include "wav192_sample_check.h"
#ifdef WAV192_DSEE_TEST
#include "wav192_dsee_config.h"
#endif
using Microsoft::WRL::ComPtr;
static HRESULT sony(const wchar_t* file,REFCLSID cls,IBaseFilter** value){
 wchar_t path[MAX_PATH];swprintf_s(path,L"C:\\Program Files (x86)\\Sony\\Music Center\\Sony.Earth\\%s",file);
 auto m=LoadLibraryExW(path,nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
 auto get=m?reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID,REFIID,void**)>(GetProcAddress(m,"DllGetClassObject")):nullptr;
 ComPtr<IClassFactory> cf;auto hr=get?get(cls,IID_PPV_ARGS(&cf)):E_FAIL;
 return SUCCEEDED(hr)?cf->CreateInstance(nullptr,IID_PPV_ARGS(value)):hr;
}
static int calls=0;
static HRESULT failSecond(IGraphBuilder* g,IPin* a,IPin* b,const AM_MEDIA_TYPE* mt){return ++calls==2?E_FAIL:g->ConnectDirect(a,b,mt);}
static HRESULT edge(IGraphBuilder* graph,IBaseFilter* from,IBaseFilter* to,bool pcm=false){
 auto a=wav192::uniquePin(from,PINDIR_OUTPUT),b=wav192::uniquePin(to,PINDIR_INPUT);
 if(!a||!b)return E_FAIL;wav192::Type mt;ComPtr<IEnumMediaTypes> e;AM_MEDIA_TYPE* raw=nullptr;
 if(pcm){if(FAILED(a->EnumMediaTypes(&e))||e->Next(1,&raw,nullptr)!=S_OK)return E_FAIL;mt.value=*raw;CoTaskMemFree(raw);mt.value.subtype=MEDIASUBTYPE_PCM;}
 return graph->ConnectDirect(a.Get(),b.Get(),pcm?&mt.value:nullptr);
}
int wmain(int argc,wchar_t** argv){
 if(argc<2)return 2;auto co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(co))return 3;
 int status=0;{
 bool low=false;
#ifdef WAV192_DSEE_TEST
 low=argc>2&&wcscmp(argv[2],L"low")==0;
#endif
 ComPtr<IGraphBuilder> graph;ComPtr<IBaseFilter> source,parser,dsee,seam,eq,renderer,tap;
 HRESULT hr=CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&graph));
 if(SUCCEEDED(hr))hr=CoCreateInstance(CLSID_AsyncReader,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&source));
 bool standard=argc>3&&wcscmp(argv[3],L"standard")==0;
 bool noSeam=argc>4&&wcscmp(argv[4],L"noseam")==0;
 if(SUCCEEDED(hr))hr=standard?CoCreateInstance(wav192::standardParserId,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&parser)):sony(L"SonyWavParser2.ax",wav192::parserId,&parser);
#ifdef WAV192_DSEE_TEST
 if(SUCCEEDED(hr))hr=sony(L"OmgDseeHxFilter.ax",wav192::dseeId,&dsee);
 if(SUCCEEDED(hr))hr=configureTestDsee(dsee.Get(),low);
 printf("DSEE_CONFIG hr=%08lx low=%d\n",ULONG(hr),low?1:0);
#endif
 if(SUCCEEDED(hr))hr=sony(L"SeamlessFilter.ax",wav192::seamlessId,&seam);
 if(SUCCEEDED(hr))hr=sony(L"OmgEqxlFilter.ax",wav192::eqId,&eq);
 if(SUCCEEDED(hr))hr=CoCreateInstance(CLSID_DSoundRender,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&renderer));
 std::vector<IBaseFilter*> all{source.Get(),parser.Get()};
 if(dsee)all.push_back(dsee.Get());if(!noSeam)all.push_back(seam.Get());all.push_back(eq.Get());all.push_back(renderer.Get());
 for(auto f:all)if(SUCCEEDED(hr))hr=graph->AddFilter(f,nullptr);
 ComPtr<IFileSourceFilter> file;if(SUCCEEDED(hr))hr=source.As(&file);if(SUCCEEDED(hr))hr=file->Load(argv[1],nullptr);
 for(size_t i=0;i+1<all.size()&&SUCCEEDED(hr);++i)hr=edge(graph.Get(),all[i],all[i+1],i==1);
 printf("BUILD hr=%08lx\n",ULONG(hr));
 bool skip=argc>2&&wcscmp(argv[2],L"skip")==0;
 bool rollback=argc>2&&wcscmp(argv[2],L"rollback")==0;
 if(SUCCEEDED(hr)&&skip){auto r=wav192::apply(graph.Get());printf("NON_TARGET outcome=%d\n",int(r.outcome));if(r.outcome!=wav192::Outcome::Skipped)status=11;}
 if(SUCCEEDED(hr)&&!skip){
  auto r=wav192::apply(graph.Get(),rollback?failSecond:wav192::connectDirect);
  printf("REWRITE outcome=%d change=%08lx restore=%08lx\n",int(r.outcome),ULONG(r.change),ULONG(r.restore));
  if(r.outcome!=((rollback||low)?wav192::Outcome::Restored:wav192::Outcome::Applied))status=4;
  // Confirm the SAME EQ object is retained but only connected after rollback.
  auto eqIn=wav192::uniquePin(eq.Get(),PINDIR_INPUT);ComPtr<IPin> peer;
  bool eqConnected=SUCCEEDED(eqIn->ConnectedTo(&peer));printf("EQ_CONNECTED %d\n",eqConnected?1:0);
  if(eqConnected!=(rollback||low))status=5;
  if(!rollback&&!low){auto again=wav192::apply(graph.Get());printf("SECOND_APPLY outcome=%d\n",int(again.outcome));if(again.outcome!=wav192::Outcome::AlreadyNative)status=6;}
  if(low){auto p=wav192::uniquePin(renderer.Get(),PINDIR_INPUT);wav192::Type mt;auto restored=p->ConnectionMediaType(&mt.value);if(FAILED(restored)||mt.value.cbFormat<16)status=12;else{auto wave=reinterpret_cast<WAVEFORMATEX*>(mt.value.pbFormat);printf("RESTORED_FORMAT rate=%lu bits=%u\n",wave->nSamplesPerSec,wave->wBitsPerSample);if(wave->nSamplesPerSec!=44100||wave->wBitsPerSample!=16)status=12;}}
 }
 ComPtr<IMediaControl> control;ComPtr<IMediaSeeking> seeking;
 if(SUCCEEDED(hr))hr=graph.As(&control);if(SUCCEEDED(hr))hr=graph.As(&seeking);
 SampleCheck* check=new SampleCheck(argv[1]);ComPtr<IGrabber> grab;
 if(SUCCEEDED(hr)&&!status&&!skip&&!low){
  CLSID id;CLSIDFromString(L"{C1F400A0-3F08-11D3-9F0B-006008039E37}",&id);
  hr=CoCreateInstance(id,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&tap));
  if(SUCCEEDED(hr))hr=graph->AddFilter(tap.Get(),L"PCM validation");
  auto upstream=rollback?eq.Get():(noSeam?parser.Get():seam.Get());auto a=wav192::uniquePin(upstream,PINDIR_OUTPUT),b=wav192::uniquePin(renderer.Get(),PINDIR_INPUT);
  if(SUCCEEDED(hr)){graph->Disconnect(a.Get());graph->Disconnect(b.Get());hr=edge(graph.Get(),upstream,tap.Get());}
  if(SUCCEEDED(hr))hr=edge(graph.Get(),tap.Get(),renderer.Get());
  if(SUCCEEDED(hr))hr=tap.As(&grab);if(SUCCEEDED(hr))hr=grab->SetCallback(check,0);
  LONGLONG start=300000000;
  if(argc>2&&wcscmp(argv[2],L"tail")==0)start=LONGLONG(((check->fileFrames()-768000)*10000000ULL+191999)/192000);
  check->segmentSeconds=noSeam?0.0:double(start)/1e7;
  if(SUCCEEDED(hr))hr=seeking->SetPositions(&start,AM_SEEKING_AbsolutePositioning,nullptr,AM_SEEKING_NoPositioning);
  if(SUCCEEDED(hr))hr=control->Run();printf("RUN hr=%08lx\n",ULONG(hr));
  if(SUCCEEDED(hr)){Sleep(1500);auto whileRunning=wav192::apply(graph.Get());printf("WHILE_RUNNING outcome=%d\n",int(whileRunning.outcome));if(whileRunning.outcome!=wav192::Outcome::Skipped)status=7;Sleep(3500);}
  control->Stop();if(grab)grab->SetCallback(nullptr,0);
  printf("PCM samples=%llu bytes=%llu mismatch=%llu errors=%llu gaps=%llu last=%llu total=%llu\n",check->samples.load(),check->bytes.load(),check->mismatches.load(),check->errors.load(),check->discontinuities.load(),check->lastFrame.load(),check->fileFrames());
  if(!check->samples||check->mismatches||check->errors||check->discontinuities)status=8;
  if(argc>2&&wcscmp(argv[2],L"tail")==0&&check->lastFrame!=check->fileFrames())status=9;
 }
 check->Release();if(FAILED(hr)&&!status)status=10;
 }CoUninitialize();printf("RESULT %d\n",status);return status;
}
