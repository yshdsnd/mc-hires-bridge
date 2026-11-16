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

struct Format { DWORD rate=0; WORD bits=0, channels=0; };
enum class Route { Unknown, Normal, Dsee, Flac24 };
struct Playback {
    const wchar_t* codec=L"";
    Format input{}, output{};
    Route route=Route::Unknown;
};
inline SRWLOCK playbackLock=SRWLOCK_INIT;
inline Playback playback;
inline void publish(Playback value) {
    AcquireSRWLockExclusive(&playbackLock);
    playback=value;
    ReleaseSRWLockExclusive(&playbackLock);
}
inline Playback snapshot() {
    AcquireSRWLockShared(&playbackLock);
    const auto value=playback;
    ReleaseSRWLockShared(&playbackLock);
    return value;
}
inline std::wstring formatText(Format f) {
    if(!f.rate || !f.bits || !f.channels)return L"\u4e0d\u660e";
    wchar_t text[80];
    if(f.rate%1000==0)
        swprintf_s(text,L"%lu kHz / %u bit / %u ch",f.rate/1000,unsigned(f.bits),unsigned(f.channels));
    else
        swprintf_s(text,L"%lu.%03lu kHz / %u bit / %u ch",f.rate/1000,f.rate%1000,unsigned(f.bits),unsigned(f.channels));
    std::wstring result=text;
    if(const auto dot=result.find(L'.');dot!=std::wstring::npos) {
        auto end=result.find(L' ',dot);
        while(end>dot+1 && result[end-1]==L'0')result.erase(--end,1);
    }
    return result;
}
inline std::wstring tooltipText(Playback p) {
    if(p.route==Route::Unknown)return L"\u518d\u751f\u60c5\u5831\u3092\u5f85\u6a5f\u4e2d";
    const wchar_t* route=L"Music Center \u306e\u901a\u5e38\u7d4c\u8def";
    if(p.route==Route::Dsee)route=L"DSEE HX \u2192 DirectSound";
    if(p.route==Route::Flac24)route=L"FLAC 24 bit \u7dad\u6301";
    std::wstring text=L"\u5165\u529b  ";
    if(*p.codec){text+=p.codec;text+=L"  ";}
    text+=formatText(p.input);
    text+=L"\r\n\u51e6\u7406  ";
    text+=route;
    text+=L"\r\nDirectSound  ";
    text+=formatText(p.output);
    return text;
}

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
inline std::string savedConfiguration() {
    wchar_t directory[MAX_PATH];if(FAILED(SHGetFolderPathW(nullptr,CSIDL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,directory)))return {};
    std::wstring path=directory;path+=L"\\Sony\\Music Center\\config\\registry.json";
    HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(f==INVALID_HANDLE_VALUE)return {};
    LARGE_INTEGER size{};bool ok=GetFileSizeEx(f,&size)&&size.QuadPart>0&&size.QuadPart<=1024*1024;std::string data;if(ok){data.resize(size_t(size.QuadPart));DWORD read=0;ok=ReadFile(f,data.data(),DWORD(data.size()),&read,nullptr)&&read==data.size();}CloseHandle(f);return ok?data:std::string{};
}
inline bool directSoundSelected() { return defaultMode(savedConfiguration()); }
inline bool dseeSharedEnabled(std::string_view data) {
    const auto player=member(member(data,"private"),"player");
    return defaultMode(data) && member(player,"dsee-state")=="true";
}

inline bool dseeSharedRequested() {
    wchar_t envBuf[16]{};
    DWORD n = GetEnvironmentVariableW(L"MC_BRIDGE_DSEE_ENABLED", envBuf, 16);
    if (n > 0) {
        if (_wcsicmp(envBuf, L"0") == 0 || _wcsicmp(envBuf, L"false") == 0 || _wcsicmp(envBuf, L"off") == 0) return false;
        if (_wcsicmp(envBuf, L"1") == 0 || _wcsicmp(envBuf, L"true") == 0 || _wcsicmp(envBuf, L"on") == 0) return true;
    }
    return dseeSharedEnabled(savedConfiguration());
}
inline HWND owner=nullptr,badge=nullptr;inline HFONT font=nullptr, tipFont=nullptr;inline UINT currentDpi=0;
inline BOOL CALLBACK findOwner(HWND w,LPARAM result) {DWORD pid=0;GetWindowThreadProcessId(w,&pid);wchar_t name[128];GetClassNameW(w,name,128);if(pid==GetCurrentProcessId()&&IsWindowVisible(w)&&!GetWindow(w,GW_OWNER)&&wcscmp(name,L"Chrome_WidgetWin_1")==0){*reinterpret_cast<HWND*>(result)=w;return FALSE;}return TRUE;}
// Child coordinates stay relative to the parent. Moving the main window never
// requires a timer tick, screen-coordinate conversion, or a separate popup move.
inline void layout() {
    if(!IsWindow(owner)||!IsWindow(badge))return;
    auto context=SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(owner));
    UINT dpi=GetDpiForWindow(owner);if(!dpi)dpi=96;
    if(dpi!=currentDpi){if(font)DeleteObject(font);font=CreateFontW(-MulDiv(12,dpi,96),0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");if(tipFont)DeleteObject(tipFont);tipFont=CreateFontW(-MulDiv(14,dpi,96),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");currentDpi=dpi;InvalidateRect(badge,nullptr,TRUE);}
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
inline bool seekTooltipMayOverlap(ULONGLONG now, ULONGLONG& hideUntil) {
    // Music Center 2.7.4 renders the tooltip in Chromium, below the native badge.
    // Its seek track is centered near y=89 and the tooltip extends right of the cursor.
    auto context = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(owner));
    POINT cursor{};
    RECT badgeRect{};
    if (GetCursorPos(&cursor) && GetAncestor(WindowFromPoint(cursor), GA_ROOT) == owner &&
        GetWindowRect(badge, &badgeRect) && ScreenToClient(owner, &cursor)) {
        MapWindowPoints(HWND_DESKTOP, owner, reinterpret_cast<POINT*>(&badgeRect), 2);
        const UINT dpi = currentDpi ? currentDpi : 96;
        if (cursor.y >= MulDiv(81, dpi, 96) && cursor.y < MulDiv(96, dpi, 96)) {
            const bool overlaps = cursor.x >= badgeRect.left - MulDiv(80, dpi, 96) &&
                                  cursor.x < badgeRect.right;
            // Keep the badge clear while the tooltip fades after leaving the track.
            hideUntil = overlaps ? now + 200 : 0;
        }
    }
    if (context) SetThreadDpiAwarenessContext(context);
    return now < hideUntil;
}
inline LRESULT CALLBACK windowProc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_PAINT){PAINTSTRUCT ps;auto dc=BeginPaint(w,&ps);RECT r;GetClientRect(w,&r);FillRect(dc,&r,reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));auto old=SelectObject(dc,font?font:GetStockObject(DEFAULT_GUI_FONT));SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(70,70,70));DrawTextW(dc,label(state.load()),-1,&r,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);SelectObject(dc,old);EndPaint(w,&ps);return 0;}
    if(msg==WM_NCHITTEST)return HTTRANSPARENT;
    if(msg==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
    if(msg==WM_DPICHANGED_AFTERPARENT){layout();return 0;}
    if(msg==WM_DESTROY){PostQuitMessage(0);return 0;}
    return DefWindowProcW(w,msg,wp,lp);
}
inline LRESULT CALLBACK tooltipProc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        auto dc=BeginPaint(w,&paint);
        RECT rect{};
        GetClientRect(w,&rect);
        FillRect(dc,&rect,GetSysColorBrush(COLOR_INFOBK));
        FrameRect(dc,&rect,GetSysColorBrush(COLOR_INFOTEXT));
        InflateRect(&rect,-MulDiv(8,currentDpi,96),-MulDiv(4,currentDpi,96));
        wchar_t text[512]{};
        GetWindowTextW(w,text,_countof(text));
        auto old=SelectObject(dc,tipFont?tipFont:GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,GetSysColor(COLOR_INFOTEXT));
        DrawTextW(dc,text,-1,&rect,DT_NOPREFIX);
        SelectObject(dc,old);
        EndPaint(w,&paint);
        return 0;
    }
    case WM_NCHITTEST:return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
    }
    return DefWindowProcW(w,msg,wp,lp);
}
struct HoverTip {
    HWND window=nullptr;
    std::wstring content;
    ULONGLONG entered=0;
    ~HoverTip(){if(IsWindow(window))DestroyWindow(window);}
    void update(HINSTANCE instance, bool enabled, ULONGLONG now) {
        POINT cursor{};
        RECT rect{};
        const auto context=SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(owner));
        const bool hover=enabled && IsWindowVisible(badge) && GetCursorPos(&cursor) &&
            GetWindowRect(badge,&rect) && PtInRect(&rect,cursor) &&
            GetAncestor(WindowFromPoint(cursor),GA_ROOT)==owner;
        if(!hover) {
            if(IsWindowVisible(window))ShowWindow(window,SW_HIDE);
            entered=0;
        } else {
            if(!entered)entered=now;
            if(now-entered>=450)show(instance,rect);
        }
        if(context)SetThreadDpiAwarenessContext(context);
    }
    void show(HINSTANCE instance, RECT anchor) {
        if(!window) {
            window=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_NOPARENTNOTIFY,L"MC_HiRes_Bridge_Tooltip",nullptr,
                WS_CHILD|WS_CLIPSIBLINGS,0,0,0,0,owner,nullptr,instance,nullptr);
            if(!window)return;
            if(!SetLayeredWindowAttributes(window,0,255,LWA_ALPHA)) { DestroyWindow(window); window=nullptr; return; }
        }
        auto next=tooltipText(snapshot());
        if(content!=next){
            content=std::move(next);
            SetWindowTextW(window,content.c_str());
            InvalidateRect(window,nullptr,FALSE);
        }
        RECT size{};
        auto dc=GetDC(window);
        if(!dc)return;
        auto old=SelectObject(dc,tipFont?tipFont:GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(dc,content.c_str(),-1,&size,DT_CALCRECT|DT_NOPREFIX);
        SelectObject(dc,old);
        ReleaseDC(window,dc);

        MapWindowPoints(HWND_DESKTOP,owner,reinterpret_cast<POINT*>(&anchor),2);
        RECT client{}, previous{};
        GetClientRect(owner,&client);
        const int width=size.right+MulDiv(16,currentDpi,96);
        const int height=size.bottom+MulDiv(8,currentDpi,96);
        const int x=max(0,min(anchor.left,client.right-width));
        // Stay below the seek bar, and hide as soon as the pointer leaves the badge.
        const int y=anchor.bottom+MulDiv(27,currentDpi,96);
        GetWindowRect(window,&previous);
        MapWindowPoints(HWND_DESKTOP,owner,reinterpret_cast<POINT*>(&previous),2);
        if(!IsWindowVisible(window) || previous.left!=x || previous.top!=y ||
           previous.right-previous.left!=width || previous.bottom-previous.top!=height)
            SetWindowPos(window,HWND_TOP,x,y,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
    }
};
inline void runUi(HINSTANCE instance) {
    WNDCLASSW wc{};wc.lpfnWndProc=windowProc;wc.hInstance=instance;wc.lpszClassName=L"MC_HiRes_Bridge_Status";wc.hbrBackground=reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));RegisterClassW(&wc);
    wc.lpfnWndProc=tooltipProc;
    wc.lpszClassName=L"MC_HiRes_Bridge_Tooltip";
    RegisterClassW(&wc);
    wc.lpszClassName=L"MC_HiRes_Bridge_Status";
    // Out-of-context callbacks run on this UI thread; never query the audio graph here.
    auto locationHook=SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE,EVENT_OBJECT_LOCATIONCHANGE,nullptr,ownerLocation,GetCurrentProcessId(),0,WINEVENT_OUTOFCONTEXT);
    HoverTip tip;
    State last=State::Off;
    ULONGLONG nextModeCheck=0, hideUntil=0;
    auto timer=SetTimer(nullptr,0,30,nullptr);MSG msg;
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
            const auto nowTick=GetTickCount64();
            if(nowTick>=nextModeCheck){
                if(!directSoundSelected()){state.store(State::Off);publish({});}
                nextModeCheck=nowTick+1000;
            }
            // Parent movement/visibility/minimization are automatic for WS_CHILD.
            const bool show=IsWindowEnabled(owner)&&!seekTooltipMayOverlap(nowTick,hideUntil);
            const bool visible=(GetWindowLongPtrW(badge,GWL_STYLE)&WS_VISIBLE)!=0;
            if(show!=visible)ShowWindow(badge,show?SW_SHOWNOACTIVATE:SW_HIDE);
            tip.update(instance,show,nowTick);
            auto now=state.load();if(now!=last){SetWindowTextW(badge,label(now));InvalidateRect(badge,nullptr,TRUE);last=now;}
        }
        TranslateMessage(&msg);DispatchMessageW(&msg);
    }
    if(timer)KillTimer(nullptr,timer);
    if(locationHook)UnhookWinEvent(locationHook);
    if(IsWindow(badge))DestroyWindow(badge);badge=nullptr;
    if(font){DeleteObject(font);font=nullptr;}if(tipFont){DeleteObject(tipFont);tipFont=nullptr;}currentDpi=0;
}
}
