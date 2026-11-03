#include <windows.h>
#include <dshow.h>
#include <cstdio>
int wmain(int argc,wchar_t** argv){if(argc!=3)return 2;if(!LoadLibraryW(argv[1]))return 3;Sleep(2000);auto co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);IGraphBuilder* graph=nullptr;auto hr=CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&graph));if(graph){hr=graph->RenderFile(argv[2],nullptr);printf("RenderFile=%08lx\n",ULONG(hr));IMediaControl* control=nullptr;if(SUCCEEDED(hr)&&SUCCEEDED(graph->QueryInterface(IID_PPV_ARGS(&control)))){hr=control->Run();printf("Run=%08lx\n",ULONG(hr));Sleep(100);control->Stop();control->Release();}graph->Release();}if(SUCCEEDED(co))CoUninitialize();printf("PID=%lu\n",GetCurrentProcessId());return FAILED(hr)?1:0;}
