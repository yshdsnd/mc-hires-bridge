#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>
// Legacy DirectShow Sample Grabber interfaces, as defined by qedit.h.
MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85") IGrabCallback:public IUnknown {
 virtual HRESULT STDMETHODCALLTYPE SampleCB(double,IMediaSample*)=0;
 virtual HRESULT STDMETHODCALLTYPE BufferCB(double,BYTE*,long)=0;
};
MIDL_INTERFACE("6B652FFF-11FE-4FCE-92AD-0266B5D7C78F") IGrabber:public IUnknown {
 virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL)=0;
 virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE*)=0;
 virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE*)=0;
 virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL)=0;
 virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long*,long*)=0;
 virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample**)=0;
 virtual HRESULT STDMETHODCALLTYPE SetCallback(IGrabCallback*,long)=0;
};
class SampleCheck:public IGrabCallback {
 std::atomic<ULONG> refs{1};
 HANDLE input=INVALID_HANDLE_VALUE;
 uint64_t dataOffset=0,dataBytes=0;
 bool valid=false;
public:
 double segmentSeconds=30.0;
 uint64_t expectedNext=0;bool first=true;
 std::atomic<uint64_t> discontinuities{0},lastFrame{0};
 uint64_t fileFrames()const{return dataBytes/6;}
 std::atomic<uint64_t> bytes{0},mismatches{0},lowNonzero{0},errors{0},samples{0};
 explicit SampleCheck(const wchar_t* path) {
  input=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
  if(input==INVALID_HANDLE_VALUE)return;
  BYTE header[12];DWORD got=0;
  if(!ReadFile(input,header,12,&got,nullptr)||got!=12||memcmp(header,"RIFF",4)||memcmp(header+8,"WAVE",4))return;
  uint64_t offset=12;bool format=false;
  for(int i=0;i<256;++i) {
   LARGE_INTEGER where;where.QuadPart=offset;SetFilePointerEx(input,where,nullptr,FILE_BEGIN);
   BYTE chunk[8];if(!ReadFile(input,chunk,8,&got,nullptr)||got!=8)return;
   DWORD size=0;memcpy(&size,chunk+4,4);
   if(!memcmp(chunk,"fmt ",4)) {
    WAVEFORMATEX wave{};DWORD count=size<sizeof(wave)?size:sizeof(wave);
    if(!ReadFile(input,&wave,count,&got,nullptr)||got!=count)return;
    format=wave.wFormatTag==1&&wave.nSamplesPerSec==192000&&wave.wBitsPerSample==24&&wave.nChannels==2&&wave.nBlockAlign==6;
   }
   if(!memcmp(chunk,"data",4)){dataOffset=offset+8;dataBytes=size;valid=format;return;}
   offset+=8+size+(size&1);
  }
 }
 ~SampleCheck(){if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** value)override {
  if(!value)return E_POINTER;*value=nullptr;
  if(id==IID_IUnknown||id==__uuidof(IGrabCallback)){*value=static_cast<IGrabCallback*>(this);AddRef();return S_OK;}
  return E_NOINTERFACE;
 }
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
 ULONG STDMETHODCALLTYPE Release()override{auto n=--refs;if(!n)delete this;return n;}
 HRESULT STDMETHODCALLTYPE BufferCB(double,BYTE*,long)override{return E_NOTIMPL;}
 HRESULT STDMETHODCALLTYPE SampleCB(double time,IMediaSample* sample)override {
  BYTE* data=nullptr;long count=sample->GetActualDataLength();
  if(!valid||count<0||FAILED(sample->GetPointer(&data))){++errors;return S_OK;}
  // Callback times are relative to the explicitly selected seek segment.
  int64_t frame=static_cast<int64_t>((time+segmentSeconds)*192000.0+0.5);
  if(frame<0||uint64_t(frame)*6+uint64_t(count)>dataBytes){++errors;return S_OK;}
  if(!first&&uint64_t(frame)!=expectedNext)++discontinuities;
  first=false;expectedNext=uint64_t(frame)+uint64_t(count)/6;lastFrame=expectedNext;
  std::vector<BYTE> expected(count);LARGE_INTEGER where;where.QuadPart=dataOffset+uint64_t(frame)*6;
  DWORD got=0;if(!SetFilePointerEx(input,where,nullptr,FILE_BEGIN)||!ReadFile(input,expected.data(),count,&got,nullptr)||got!=DWORD(count)){++errors;return S_OK;}
  uint64_t diff=0,low=0;for(long i=0;i<count;++i){if(data[i]!=expected[i])++diff;if(i%3==0&&data[i])++low;}
  bytes+=count;mismatches+=diff;lowNonzero+=low;++samples;return S_OK;
 }
};
