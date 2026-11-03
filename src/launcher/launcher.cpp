#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <stdexcept>
#include "bridge_startup.h"

namespace {
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};
struct Error { std::wstring text; DWORD code; };
[[noreturn]] void fail(const wchar_t* message, DWORD code = GetLastError()) { throw Error{message, code}; }
std::wstring folder(const std::wstring& path) { return path.substr(0, path.find_last_of(L"\\/")); }
bool fileExists(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}
std::wstring ownFolder() {
    std::vector<wchar_t> path(32768);
    DWORD n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!n || n == path.size()) fail(L"ランチャーの場所を確認できません。", ERROR_BAD_PATHNAME);
    return folder(std::wstring(path.data(), n));
}
std::wstring musicCenterPath() {
    PWSTR programFiles = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFilesX86, 0, nullptr, &programFiles)))
        fail(L"Music Centerのインストール先を確認できません。", ERROR_PATH_NOT_FOUND);
    std::wstring path = std::wstring(programFiles) + L"\\Sony\\Music Center\\MusicCenter.exe";
    CoTaskMemFree(programFiles);
    if (!fileExists(path)) fail(L"Music Center for PCが標準の場所に見つかりません。", ERROR_FILE_NOT_FOUND);
    return path;
}
struct Target { const std::wstring& path; DWORD pid = 0; HWND window = nullptr; };
BOOL CALLBACK findWindow(HWND window, LPARAM context) {
    auto& target = *reinterpret_cast<Target*>(context);
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
    wchar_t name[128]{};
    GetClassNameW(window, name, 128);
    if (wcscmp(name, L"Chrome_WidgetWin_1")) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    wchar_t path[32768]; DWORD length = _countof(path);
    if (process && QueryFullProcessImageNameW(process.value, 0, path, &length) &&
        !_wcsicmp(path, target.path.c_str())) {
        target.pid = pid; target.window = window; return FALSE;
    }
    return TRUE;
}
Target findTarget(const std::wstring& path) {
    Target target{path}; EnumWindows(findWindow, reinterpret_cast<LPARAM>(&target)); return target;
}
std::vector<MODULEENTRY32W> modules(DWORD pid) {
    // Module snapshots may transiently fail while the loader is changing its list.
    for (unsigned attempt = 0; attempt != 20; ++attempt) {
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
        if (!snapshot) {
            if (GetLastError() == ERROR_BAD_LENGTH) { Sleep(25); continue; }
            fail(L"Music Centerの読み込み状態を確認できません。通常の権限で起動してください。");
        }
        std::vector<MODULEENTRY32W> result;
        MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
        if (!Module32FirstW(snapshot.value, &entry)) fail(L"Music Centerのモジュールを取得できません。");
        do { result.push_back(entry); } while (Module32NextW(snapshot.value, &entry));
        return result;
    }
    fail(L"Music Centerの読み込み状態が安定しません。もう一度起動してください。", ERROR_RETRY);
}
bool bridgeLoaded(DWORD pid) {
    for (const auto& module : modules(pid)) if (!_wcsicmp(module.szModule, L"flac24_override.dll")) return true;
    return false;
}
void loadBridge(HANDLE process, DWORD pid, const std::wstring& path) {
    USHORT target = 0, native = 0, self = 0, selfNative = 0;
    if (!IsWow64Process2(process, &target, &native) ||
        !IsWow64Process2(GetCurrentProcess(), &self, &selfNative)) fail(L"アプリの種類を確認できません。");
    if (target != self || native != selfNative)
        fail(L"このランチャーは32bit版Music Center専用です。", ERROR_BAD_EXE_FORMAT);
    auto load = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE owner = nullptr; wchar_t ownerPath[MAX_PATH]{};
    if (!load || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(load), &owner)) fail(L"ブリッジを読み込む準備ができません。");
    GetModuleFileNameW(owner, ownerPath, MAX_PATH);
    auto name = wcsrchr(ownerPath, L'\\'); name = name ? name + 1 : ownerPath;
    BYTE* remoteBase = nullptr;
    for (const auto& module : modules(pid)) if (!_wcsicmp(module.szModule, name)) { remoteBase = module.modBaseAddr; break; }
    if (!remoteBase) fail(L"Music Centerの読み込み機能が見つかりません。", ERROR_MOD_NOT_FOUND);
    SIZE_T size = (path.size() + 1) * sizeof(wchar_t);
    void* argument = VirtualAllocEx(process, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!argument) fail(L"ブリッジの読み込み領域を確保できません。");
    if (!WriteProcessMemory(process, argument, path.c_str(), size, nullptr)) {
        DWORD error = GetLastError(); VirtualFreeEx(process, argument, 0, MEM_RELEASE);
        fail(L"ブリッジの場所をMusic Centerへ渡せません。", error);
    }
    auto entry = reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteBase +
        (reinterpret_cast<BYTE*>(load) - reinterpret_cast<BYTE*>(owner)));
    Handle thread(CreateRemoteThread(process, nullptr, 0, entry, argument, 0, nullptr));
    if (!thread) {
        DWORD error = GetLastError(); VirtualFreeEx(process, argument, 0, MEM_RELEASE);
        fail(L"Music Centerへブリッジを読み込めません。", error);
    }
    if (WaitForSingleObject(thread.value, 15000) != WAIT_OBJECT_0) {
        // The remote loader may still use this path. Retain it until target exit.
        fail(L"ブリッジの読み込みが時間内に終わりませんでした。Music Centerを終了してから再実行してください。", WAIT_TIMEOUT);
    }
    DWORD result = 0; GetExitCodeThread(thread.value, &result);
    VirtualFreeEx(process, argument, 0, MEM_RELEASE);
    if (!result) fail(L"ブリッジDLLを読み込めません。EXEとDLLを同じフォルダーに置いてください。", ERROR_DLL_INIT_FAILED);
}
int launch() {
    Handle mutex(CreateMutexW(nullptr, FALSE, L"Local\\MC_HiRes_Bridge_Launcher"));
    if (!mutex) fail(L"ランチャーを開始できません。");
    DWORD acquired = WaitForSingleObject(mutex.value, 0);
    if (acquired == WAIT_TIMEOUT) return 0; // Another double-click is already handling startup.
    if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) fail(L"ランチャーの状態を確認できません。");
    struct Unlock { HANDLE h; ~Unlock() { ReleaseMutex(h); } } unlock{mutex.value};
    const auto dll = ownFolder() + L"\\flac24_override.dll";
    if (!fileExists(dll)) fail(L"flac24_override.dllが見つかりません。ランチャーと同じフォルダーに置いてください。", ERROR_FILE_NOT_FOUND);
    const auto app = musicCenterPath();
    auto target = findTarget(app);
    if (!target.pid) {
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_SHOWNORMAL;
        PROCESS_INFORMATION created{};
        std::wstring command = L"\"" + app + L"\"";
        const auto directory = folder(app);
        if (!CreateProcessW(app.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
            directory.c_str(), &startup, &created)) fail(L"Music Centerを起動できません。");
        Handle createdProcess(created.hProcess), createdThread(created.hThread);
        WaitForInputIdle(createdProcess.value, 1000);
        const auto deadline = GetTickCount64() + 30000;
        do {
            auto candidate = findTarget(app);
            target.pid = candidate.pid; target.window = candidate.window;
            if (target.pid) break;
            Sleep(100);
        } while (GetTickCount64() < deadline);
        if (!target.pid) fail(L"Music Centerの画面が見つかりません。起動状態を確認して再実行してください。", WAIT_TIMEOUT);
    }
    Handle process(OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, target.pid));
    if (!process) fail(L"Music Centerへ接続できません。Music Centerとランチャーを通常の権限で起動してください。");
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, bridgeStartup::eventName(L"Ready", target.pid).c_str()));
    Handle failed(CreateEventW(nullptr, TRUE, FALSE, bridgeStartup::eventName(L"Failed", target.pid).c_str()));
    if (!ready || !failed) fail(L"ブリッジの初期化通知を準備できません。");
    if (bridgeLoaded(target.pid)) {
        // Never install a second copy over active hooks, even from another folder.
        if (WaitForSingleObject(ready.value, 0) != WAIT_OBJECT_0)
            fail(L"ブリッジがすでに読み込まれていますが、初期化完了を確認できません。Music Centerを終了してから、このランチャーを再実行してください。", ERROR_ALREADY_EXISTS);
    } else loadBridge(process.value, target.pid, dll);
    HANDLE waits[] = {ready.value, failed.value, process.value};
    DWORD initialized = WaitForMultipleObjects(_countof(waits), waits, FALSE, 20000);
    if (initialized == WAIT_OBJECT_0 + 1)
        fail(L"ブリッジの初期化に失敗しました。対応するMusic Centerのバージョンか確認してください。", ERROR_DLL_INIT_FAILED);
    if (initialized == WAIT_OBJECT_0 + 2)
        fail(L"ブリッジの準備中にMusic Centerが終了しました。", ERROR_PROCESS_ABORTED);
    if (initialized != WAIT_OBJECT_0)
        fail(L"ブリッジの準備完了を確認できません。Music Centerを終了してから再実行してください。", initialized == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError());
    if (IsIconic(target.window)) ShowWindowAsync(target.window, SW_RESTORE);
    SetForegroundWindow(target.window);
    return 0;
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try { return launch(); }
    catch (const Error& error) {
        auto message = error.text + L"\n\nエラー番号: " + std::to_wstring(error.code);
        MessageBoxW(nullptr, message.c_str(), L"MC HiRes Bridge", MB_OK | MB_ICONERROR);
    }
    catch (...) { MessageBoxW(nullptr, L"ランチャーの処理に失敗しました。", L"MC HiRes Bridge", MB_OK | MB_ICONERROR); }
    return 1;
}
