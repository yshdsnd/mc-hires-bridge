#pragma once
#include <windows.h>
#include <atomic>
#include <string>
#include <string_view>
#include <shlobj.h>

namespace bridgeStatus {
enum class State { Ready, On, Off };
inline std::atomic<State> state{State::Ready};
inline const wchar_t* label(State s) { return s==State::Ready?L"HR-BR RDY":s==State::On?L"HR-BR ON":L"HR-BR OFF"; }

// Read only the saved output-mode property. Missing/invalid configuration is OFF.
// Bound both input size and nesting; do not search for an unrelated "mode" key.
inline size_t space(std::string_view s,size_t p) { while(p<s.size()&&(s[p]==' '||s[p]=='\r'||s[p]=='\n'||s[p]=='\t'))++p;return p; }
inline size_t endString(std::string_view s,size_t p) { if(p>=s.size()||s[p]!='"')return std::string_view::npos;for(++p;p<s.size();++p){if(s[p]=='\\'){if(++p==s.size())break;}else if(s[p]=='"')return p+1;}return std::string_view::npos; }
inline size_t endValue(std::string_view s,size_t p,unsigned depth=0) {
    p=space(s,p);if(p>=s.size()||depth>32)return std::string_view::npos;
    if(s[p]=='"')return endString(s,p);
    if(s[p]=='{'||s[p]=='['){char close=s[p]=='{'?'}':']';for(++p;p<s.size();){p=space(s,p);if(p==s.size())break;if(s[p]==close)return p+1;if(s[p]==','||s[p]==':'){++p;continue;}auto end=endValue(s,p,depth+1);if(end==std::string_view::npos||end<=p)return std::string_view::npos;p=end;}return std::string_view::npos;}
    auto begin=p;while(p<s.size()&&s[p]!=','&&s[p]!='}'&&s[p]!=']'&&s[p]!=' '&&s[p]!='\r'&&s[p]!='\n'&&s[p]!='\t')++p;return p>begin?p:std::string_view::npos;
}
inline std::string_view member(std::string_view s,std::string_view key) {
    size_t p=space(s,0);if(p==s.size()||s[p++]!='{')return {};std::string_view found;
    for(;;){p=space(s,p);if(p>=s.size())return {};if(s[p]=='}')return found;auto end=endString(s,p);if(end==std::string_view::npos)return {};bool match=s.substr(p+1,end-p-2)==key;p=space(s,end);if(p>=s.size()||s[p++]!=':')return {};p=space(s,p);end=endValue(s,p);if(end==std::string_view::npos)return {};if(match){if(!found.empty())return {};found=s.substr(p,end-p);}p=space(s,end);if(p>=s.size())return {};if(s[p]=='}')return found;if(s[p++]!=',')return {};}
}
inline bool defaultMode(std::string_view json) {auto value=member(member(member(member(json,"private"),"audio-output-device"),"output-devices"),"mode");return value=="\"default\"";}
inline bool directSoundSelected() {
    wchar_t directory[MAX_PATH];if(FAILED(SHGetFolderPathW(nullptr,CSIDL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,directory)))return false;
    std::wstring path=directory;path+=L"\\Sony\\Music Center\\config\\registry.json";
    HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(f==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size{};bool ok=GetFileSizeEx(f,&size)&&size.QuadPart>0&&size.QuadPart<=1024*1024;std::string data;if(ok){data.resize(size_t(size.QuadPart));DWORD read=0;ok=ReadFile(f,data.data(),DWORD(data.size()),&read,nullptr)&&read==data.size();}CloseHandle(f);return ok&&defaultMode(data);
}
inline HWND owner=nullptr,badge=nullptr;inline HFONT font=nullptr;inline UINT currentDpi=0;
inline BOOL CALLBACK findOwner(HWND w,LPARAM result) {DWORD pid=0;GetWindowThreadProcessId(w,&pid);wchar_t name[128];GetClassNameW(w,name,128);if(pid==GetCurrentProcessId()&&IsWindowVisible(w)&&!GetWindow(w,GW_OWNER)&&wcscmp(name,L"Chrome_WidgetWin_1")==0){*reinterpret_cast<HWND*>(result)=w;return FALSE;}return TRUE;}
// Child coordinates stay relative to the parent. Moving the main window never
// requires a timer tick, screen-coordinate conversion, or a separate popup move.
inline void layout() {
    if(!IsWindow(owner)||!IsWindow(badge))return;
    auto context=SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(owner));
    UINT dpi=GetDpiForWindow(owner);if(!dpi)dpi=96;
    if(dpi!=currentDpi){if(font)DeleteObject(font);font=CreateFontW(-MulDiv(12,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");currentDpi=dpi;InvalidateRect(badge,nullptr,TRUE);}
    RECT r;GetClientRect(owner,&r);
    int x=max(0,r.right-MulDiv(380,dpi,96)),y=MulDiv(48,dpi,96),width=MulDiv(92,dpi,96),height=MulDiv(23,dpi,96);
    RECT existing;GetWindowRect(badge,&existing);MapWindowPoints(HWND_DESKTOP,owner,reinterpret_cast<POINT*>(&existing),2);
    if(existing.left!=x||existing.top!=y||existing.right-existing.left!=width||existing.bottom-existing.top!=height)
        SetWindowPos(badge,HWND_TOP,x,y,width,height,SWP_NOACTIVATE);
    if(context)SetThreadDpiAwarenessContext(context);
}
inline void CALLBACK ownerLocation(HWINEVENTHOOK,DWORD,HWND w,LONG object,LONG child,DWORD,DWORD) {
    if(w==owner&&object==OBJID_WINDOW&&child==CHILDID_SELF)layout();
}
inline LRESULT CALLBACK windowProc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_PAINT){PAINTSTRUCT ps;auto dc=BeginPaint(w,&ps);RECT r;GetClientRect(w,&r);FillRect(dc,&r,reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));auto old=SelectObject(dc,font?font:GetStockObject(DEFAULT_GUI_FONT));SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(70,70,70));DrawTextW(dc,label(state.load()),-1,&r,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);SelectObject(dc,old);EndPaint(w,&ps);return 0;}
    if(msg==WM_NCHITTEST)return HTTRANSPARENT;
    if(msg==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
    if(msg==WM_DPICHANGED_AFTERPARENT){layout();return 0;}
    if(msg==WM_DESTROY){PostQuitMessage(0);return 0;}
    return DefWindowProcW(w,msg,wp,lp);
}
inline void runUi(HINSTANCE instance) {
    WNDCLASSW wc{};wc.lpfnWndProc=windowProc;wc.hInstance=instance;wc.lpszClassName=L"MC_HiRes_Bridge_Status";wc.hbrBackground=reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));RegisterClassW(&wc);
    // Out-of-context callbacks run on this UI thread; never query the audio graph here.
    auto locationHook=SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE,EVENT_OBJECT_LOCATIONCHANGE,nullptr,ownerLocation,GetCurrentProcessId(),0,WINEVENT_OUTOFCONTEXT);
    State last=State::Off;unsigned ticks=0;auto timer=SetTimer(nullptr,0,250,nullptr);MSG msg;
    while(GetMessageW(&msg,nullptr,0,0)>0){
        if(msg.message==WM_TIMER&&msg.wParam==timer){
            if(!IsWindow(owner)){owner=nullptr;EnumWindows(findOwner,reinterpret_cast<LPARAM>(&owner));if(!owner)continue;}
            if(!badge){
                auto context=SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(owner));
                badge=CreateWindowExW(WS_EX_LAYERED|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT|WS_EX_NOPARENTNOTIFY,wc.lpszClassName,label(state.load()),WS_CHILD|WS_CLIPSIBLINGS,0,0,1,1,owner,nullptr,instance,nullptr);
                if(badge)SetLayeredWindowAttributes(badge,0,255,LWA_ALPHA);
                if(context)SetThreadDpiAwarenessContext(context);
                layout();
            }
            if(!badge)continue;
            if(!locationHook)layout(); // Fallback for resize only; parent movement remains automatic.
            if(++ticks%4==0&&!directSoundSelected())state.store(State::Off);
            // Parent movement/visibility/minimization are automatic for WS_CHILD.
            ShowWindow(badge,IsWindowEnabled(owner)?SW_SHOWNOACTIVATE:SW_HIDE);
            auto now=state.load();if(now!=last){SetWindowTextW(badge,label(now));InvalidateRect(badge,nullptr,TRUE);last=now;}
        }
        TranslateMessage(&msg);DispatchMessageW(&msg);
    }
    if(timer)KillTimer(nullptr,timer);
    if(locationHook)UnhookWinEvent(locationHook);
    if(IsWindow(badge))DestroyWindow(badge);badge=nullptr;
    if(font){DeleteObject(font);font=nullptr;}currentDpi=0;
}
}
