#pragma once
#include <windows.h>
#include <dshow.h>
#include <dsound.h>
#include <cstdint>
#include <atomic>

namespace syncTailDeferral {

// DirectShow DirectSoundRender interface to access the secondary buffer
static const IID kIID_IAMDirectSound = {0x546F4260, 0xD0DE, 0x11CE, {0x8C, 0xE7, 0x00, 0xAA, 0x00, 0x68, 0x9B, 0x18}};

interface IAMDirectSound : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetDirectSoundInterface(LPDIRECTSOUND* lplpds) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPrimaryBufferInterface(LPDIRECTSOUNDBUFFER* lplpdsb) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSecondaryBufferInterface(LPDIRECTSOUNDBUFFER* lplpdsb) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetFocusWindow(HWND hWnd, BOOL bDaemon) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFocusWindow(HWND* hWnd, BOOL* bDaemon) = 0;
};

inline bool safeQueryDirectSoundBuffer(IDirectSoundBuffer* buf, DWORD* playCursor, DWORD* writeCursor, DWORD* status, HRESULT* hrPos, HRESULT* hrStat) {
    if (!buf) {
        if (hrPos) *hrPos = E_POINTER;
        if (hrStat) *hrStat = E_POINTER;
        return false;
    }
    __try {
        HRESULT pRes = buf->GetCurrentPosition(playCursor, writeCursor);
        if (hrPos) *hrPos = pRes;
        HRESULT sRes = buf->GetStatus(status);
        if (hrStat) *hrStat = sRes;
        return SUCCEEDED(pRes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (hrPos) *hrPos = HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS);
        if (hrStat) *hrStat = HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS);
        return false;
    }
}

inline IDirectSoundBuffer* findDirectSoundBufferFromGraph(IFilterGraph* graph) {
    if (!graph) return nullptr;
    IDirectSoundBuffer* foundBuffer = nullptr;
    IEnumFilters* filters = nullptr;
    if (SUCCEEDED(graph->EnumFilters(&filters))) {
        IBaseFilter* f = nullptr;
        while (filters->Next(1, &f, nullptr) == S_OK) {
            IAMDirectSound* amDs = nullptr;
            if (SUCCEEDED(f->QueryInterface(kIID_IAMDirectSound, reinterpret_cast<void**>(&amDs))) && amDs) {
                LPDIRECTSOUNDBUFFER dsb = nullptr;
                if (SUCCEEDED(amDs->GetSecondaryBufferInterface(&dsb)) && dsb) {
                    foundBuffer = dsb;
                }
                amDs->Release();
            }
            f->Release();
            if (foundBuffer) break;
        }
        filters->Release();
    }
    return foundBuffer;
}

// Global flag to track user input (mouse/keyboard) for manual operation abortion
inline std::atomic<DWORD> g_lastUserInputTick{0};

inline void notifyUserInput() {
    g_lastUserInputTick.store(GetTickCount());
}

inline bool isManualOperationRecent(DWORD thresholdMs = 400) {
    DWORD last = g_lastUserInputTick.load();
    return (last != 0) && (GetTickCount() - last < thresholdMs);
}

// Synchronously drains DirectSound buffer at track tail during format-changing track transitions.
// Blocks inside Pause() until the renderer buffer is completely played or timeout expires.
inline bool drainTrackTailSynchronous(IUnknown* graphObject, DWORD maxTimeoutMs = 3000) {
    if (!graphObject) return false;

    IFilterGraph* graph = nullptr;
    if (FAILED(graphObject->QueryInterface(IID_PPV_ARGS(&graph))) || !graph) {
        return false;
    }

    IMediaSeeking* seeking = nullptr;
    graphObject->QueryInterface(IID_PPV_ARGS(&seeking));

    LONGLONG curPos = 0, stopPos = 0;
    DWORD remainingMs = 0;
    bool hasSeeking = (seeking && SUCCEEDED(seeking->GetCurrentPosition(&curPos)) &&
                       SUCCEEDED(seeking->GetStopPosition(&stopPos)) && stopPos > 0);

    if (hasSeeking) {
        if (stopPos > curPos) {
            remainingMs = static_cast<DWORD>((stopPos - curPos) / 10000);
        } else {
            remainingMs = 0; // Already reached or past stop position
        }
    }

    // Only defer Pause if we are near track tail (e.g. remaining <= 2500ms)
    // If remainingMs > 3000ms, this is likely a manual mid-track pause by user, not automatic track tail.
    if (hasSeeking && remainingMs > 3000 && !isManualOperationRecent()) {
        if (seeking) seeking->Release();
        graph->Release();
        return false;
    }

    IDirectSoundBuffer* dsBuf = findDirectSoundBufferFromGraph(graph);

    DWORD startTick = GetTickCount();
    DWORD waitLimit = (remainingMs > 0 && remainingMs < maxTimeoutMs) ? (remainingMs + 300) : maxTimeoutMs;
    if (waitLimit > 4000) waitLimit = 4000;

    bool drained = false;

    while (true) {
        if (isManualOperationRecent()) {
            // User initiated a manual action (click, keypress): cancel wait immediately
            break;
        }

        // 1. Check seeking position
        if (seeking && SUCCEEDED(seeking->GetCurrentPosition(&curPos)) &&
            SUCCEEDED(seeking->GetStopPosition(&stopPos))) {
            if (stopPos > 0 && curPos >= stopPos) {
                drained = true;
                break;
            }
        }

        // 2. Check DirectSound buffer status
        if (dsBuf) {
            DWORD playCursor = 0, writeCursor = 0, status = 0;
            HRESULT hrPos = S_OK, hrStat = S_OK;
            if (safeQueryDirectSoundBuffer(dsBuf, &playCursor, &writeCursor, &status, &hrPos, &hrStat)) {
                if (SUCCEEDED(hrStat) && !(status & DSBSTATUS_PLAYING)) {
                    drained = true;
                    break;
                }
            }
        }

        // 3. Check timeout
        if (GetTickCount() - startTick >= waitLimit) {
            drained = true;
            break;
        }

        // Pump Windows messages so UI does not freeze during wait
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        Sleep(10);
    }

    if (dsBuf) dsBuf->Release();
    if (seeking) seeking->Release();
    graph->Release();

    return drained;
}

} // namespace syncTailDeferral
