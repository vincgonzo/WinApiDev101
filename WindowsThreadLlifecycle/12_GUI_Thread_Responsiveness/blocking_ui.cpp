#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>

constexpr int StartControlId = 100;
constexpr UINT_PTR HeartbeatTimer = 1;
constexpr wchar_t WindowClassName[] = L"ThreadTrainingBlockingWindow";

struct ComputationResult {
    ULONGLONG checksum;
    ULONGLONG batches;
    ULONGLONG elapsedMilliseconds;
    bool cancelled;
};

struct AppState {
    HINSTANCE instance;
    HWND window;
    HWND startButton;
    HWND identityText;
    HWND statusText;
    HWND timingText;
    HWND resultText;
    bool automatic;
    bool busy;
    bool completed;
    bool failed;
    bool timerActive;
    bool closePosted;
    DWORD uiThreadId;
    ULONGLONG appStarted;
    ULONGLONG workStarted;
    ULONGLONG heartbeat;
    ULONGLONG heartbeatAtStart;
    ULONGLONG workHeartbeats;
    ULONGLONG lastHeartbeat;
    ULONGLONG largestGap;
    ComputationResult result;
    const wchar_t* status;
};

AppState g_app{};

[[noreturn]] void Fail(const char* operation, DWORD error) {
    std::fprintf(stderr, "%s failed: Win32 error %lu (0x%lx)\n", operation, error, error);
    ExitProcess(1);
}

void Check(BOOL success, const char* operation) {
    if (!success) Fail(operation, GetLastError());
}

void NameThread(HANDLE thread, const wchar_t* name) {
    using DescriptionFunction = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    HMODULE module = GetModuleHandleW(L"KernelBase.dll");
    Check(module != nullptr, "GetModuleHandleW(KernelBase)");
    const FARPROC address = GetProcAddress(module, "SetThreadDescription");
    Check(address != nullptr, "GetProcAddress(SetThreadDescription)");
    DescriptionFunction describeThread = nullptr;
    static_assert(sizeof(describeThread) == sizeof(address), "Function pointer sizes must match");
    std::memcpy(&describeThread, &address, sizeof(describeThread));
    const HRESULT result = describeThread(thread, name);
    if (FAILED(result)) Fail("SetThreadDescription", static_cast<DWORD>(result));
}

__declspec(noinline) ComputationResult ComputeFor(DWORD durationMilliseconds, LONG* cancelRequested) {
    const ULONGLONG started = GetTickCount64();
    ComputationResult result{};
    result.checksum = 0x123456789abcdefULL;
    do {
        if (cancelRequested != nullptr && InterlockedCompareExchange(cancelRequested, 0, 0) != 0) {
            result.cancelled = true;
            break;
        }
        for (DWORD step = 0; step < 4096; ++step) {
            result.checksum ^= result.checksum << 13;
            result.checksum ^= result.checksum >> 7;
            result.checksum ^= result.checksum << 17;
        }
        ++result.batches;
    } while (GetTickCount64() - started < durationMilliseconds);
    result.elapsedMilliseconds = GetTickCount64() - started;
    return result;
}

void SetText(HWND control, const wchar_t* text) {
    Check(SetWindowTextW(control, text), "SetWindowTextW");
}

void RefreshDisplay() {
    wchar_t buffer[256]{};
    int length = std::swprintf(buffer, 256, L"PID: %lu (0x%lx)\nUI TID: %lu (0x%lx)",
                              GetCurrentProcessId(), GetCurrentProcessId(), g_app.uiThreadId, g_app.uiThreadId);
    if (length < 0) Fail("Format identity", ERROR_INSUFFICIENT_BUFFER);
    SetText(g_app.identityText, buffer);
    SetText(g_app.statusText, g_app.status);
    const ULONGLONG elapsed = g_app.busy ? GetTickCount64() - g_app.workStarted : g_app.result.elapsedMilliseconds;
    length = std::swprintf(buffer, 256, L"Elapsed: %llu ms\nHeartbeat: %llu\nLargest timer gap: %llu ms",
                          static_cast<unsigned long long>(elapsed), static_cast<unsigned long long>(g_app.heartbeat),
                          static_cast<unsigned long long>(g_app.largestGap));
    if (length < 0) Fail("Format timing", ERROR_INSUFFICIENT_BUFFER);
    SetText(g_app.timingText, buffer);
    length = std::swprintf(buffer, 256, L"Checksum: %llu\nBatches: %llu\nHeartbeats during work: %llu",
                          static_cast<unsigned long long>(g_app.result.checksum),
                          static_cast<unsigned long long>(g_app.result.batches),
                          static_cast<unsigned long long>(g_app.workHeartbeats));
    if (length < 0) Fail("Format result", ERROR_INSUFFICIENT_BUFFER);
    SetText(g_app.resultText, buffer);
}

void LayoutControls(HWND window) {
    if (g_app.startButton == nullptr) return;
    RECT client{};
    Check(GetClientRect(window, &client), "GetClientRect");
    const int width = client.right > 32 ? static_cast<int>(client.right) - 32 : 1;
    Check(MoveWindow(g_app.startButton, 16, 16, 112, 32, TRUE), "MoveWindow(Start)");
    Check(MoveWindow(g_app.identityText, 16, 62, width, 60, TRUE), "MoveWindow(identity)");
    Check(MoveWindow(g_app.statusText, 16, 130, width, 40, TRUE), "MoveWindow(status)");
    Check(MoveWindow(g_app.timingText, 16, 180, width, 60, TRUE), "MoveWindow(timing)");
    Check(MoveWindow(g_app.resultText, 16, 250, width, 60, TRUE), "MoveWindow(result)");
}

HWND CreateLabel(HWND parent) {
    HWND control = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   0, 0, 1, 1, parent, nullptr, g_app.instance, nullptr);
    Check(control != nullptr, "CreateWindowExW(STATIC)");
    return control;
}

void PostClose() {
    if (g_app.closePosted) return;
    Check(PostMessageW(g_app.window, WM_CLOSE, 0, 0), "PostMessageW(WM_CLOSE)");
    g_app.closePosted = true;
}

void StartWork() {
    if (g_app.busy) return;
    g_app.busy = true;
    g_app.completed = false;
    g_app.workHeartbeats = 0;
    g_app.result = ComputationResult{};
    g_app.workStarted = GetTickCount64();
    g_app.heartbeatAtStart = g_app.heartbeat;
    g_app.status = L"Computing on the UI thread";
    EnableWindow(g_app.startButton, FALSE);
    RefreshDisplay();
    Check(RedrawWindow(g_app.window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN),
          "RedrawWindow(before work)");
    std::printf("Phase 2: WM_COMMAND computing on UI TID=%lu (0x%lx).\n", g_app.uiThreadId, g_app.uiThreadId);
    g_app.result = ComputeFor(g_app.automatic ? 1000 : 4000, nullptr);
    g_app.workHeartbeats = g_app.heartbeat - g_app.heartbeatAtStart;
    g_app.busy = false;
    g_app.completed = true;
    g_app.failed = g_app.failed || g_app.result.batches == 0 || g_app.result.checksum == 0 || g_app.workHeartbeats != 0;
    g_app.status = g_app.failed ? L"Failed" : L"Completed on the UI thread";
    EnableWindow(g_app.startButton, TRUE);
    RefreshDisplay();
    std::printf("Work ended: elapsed=%llu ms, checksum=%llu, batches=%llu, heartbeats during work=%llu.\n",
                static_cast<unsigned long long>(g_app.result.elapsedMilliseconds),
                static_cast<unsigned long long>(g_app.result.checksum),
                static_cast<unsigned long long>(g_app.result.batches),
                static_cast<unsigned long long>(g_app.workHeartbeats));
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wordParameter, LPARAM longParameter) {
    switch (message) {
    case WM_CREATE:
        g_app.window = window;
        g_app.startButton = CreateWindowExW(0, L"BUTTON", L"&Start", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                           16, 16, 112, 32, window,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(StartControlId)), g_app.instance, nullptr);
        Check(g_app.startButton != nullptr, "CreateWindowExW(Start)");
        g_app.identityText = CreateLabel(window);
        g_app.statusText = CreateLabel(window);
        g_app.timingText = CreateLabel(window);
        g_app.resultText = CreateLabel(window);
        LayoutControls(window);
        RefreshDisplay();
        Check(SetTimer(window, HeartbeatTimer, 100, nullptr) != 0, "SetTimer");
        g_app.timerActive = true;
        return 0;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(longParameter);
        limits->ptMinTrackSize.x = 600;
        limits->ptMinTrackSize.y = 400;
        return 0;
    }
    case WM_SIZE:
        if (wordParameter != SIZE_MINIMIZED) LayoutControls(window);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wordParameter) == StartControlId && HIWORD(wordParameter) == BN_CLICKED) {
            StartWork();
            return 0;
        }
        break;
    case WM_TIMER:
        if (wordParameter == HeartbeatTimer) {
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG gap = now - g_app.lastHeartbeat;
            if (gap > g_app.largestGap) g_app.largestGap = gap;
            g_app.lastHeartbeat = now;
            ++g_app.heartbeat;
            RefreshDisplay();
            if (g_app.automatic && now - g_app.appStarted > 12000) {
                g_app.failed = true;
                std::fprintf(stderr, "Automatic UI watchdog expired.\n");
                PostClose();
            } else if (g_app.automatic && g_app.completed) {
                PostClose();
            }
            return 0;
        }
        break;
    case WM_CLOSE:
        if (g_app.automatic && (!g_app.completed || g_app.heartbeat == 0)) g_app.failed = true;
        Check(DestroyWindow(window), "DestroyWindow");
        return 0;
    case WM_DESTROY:
        if (g_app.timerActive) {
            Check(KillTimer(window, HeartbeatTimer), "KillTimer");
            g_app.timerActive = false;
        }
        g_app.window = nullptr;
        PostQuitMessage(g_app.failed ? 1 : 0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wordParameter, longParameter);
}

int main(int argumentCount, char** arguments) {
    g_app.automatic = argumentCount == 2 && std::strcmp(arguments[1], "--auto") == 0;
    if (argumentCount > 1 && !g_app.automatic) {
        std::fprintf(stderr, "Usage: blocking_ui.exe [--auto]\n");
        return 1;
    }
    NameThread(GetCurrentThread(), L"blocking-ui");
    g_app.instance = GetModuleHandleW(nullptr);
    Check(g_app.instance != nullptr, "GetModuleHandleW");
    g_app.uiThreadId = GetCurrentThreadId();
    g_app.appStarted = GetTickCount64();
    g_app.lastHeartbeat = g_app.appStarted;
    g_app.status = L"Ready";
    std::printf("Phase 1: PID=%lu (0x%lx), UI/main TID=%lu (0x%lx).\n",
                GetCurrentProcessId(), GetCurrentProcessId(), g_app.uiThreadId, g_app.uiThreadId);

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = g_app.instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    Check(windowClass.hCursor != nullptr, "LoadCursorW");
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
    windowClass.lpszClassName = WindowClassName;
    Check(RegisterClassW(&windowClass) != 0, "RegisterClassW");
    HWND window = CreateWindowExW(0, WindowClassName, L"Blocking UI thread", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 720, 420, nullptr, nullptr, g_app.instance, nullptr);
    Check(window != nullptr, "CreateWindowExW(main)");
    ShowWindow(window, SW_SHOWNORMAL);
    Check(UpdateWindow(window), "UpdateWindow");
    if (g_app.automatic) {
        Check(PostMessageW(window, WM_COMMAND, MAKEWPARAM(StartControlId, BN_CLICKED),
                           reinterpret_cast<LPARAM>(g_app.startButton)), "PostMessageW(Start)");
    }

    MSG message{};
    for (;;) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result == -1) Fail("GetMessageW", GetLastError());
        if (result == 0) break;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    Check(UnregisterClassW(WindowClassName, g_app.instance), "UnregisterClassW");
    std::printf("Phase 3: window closed; heartbeat=%llu, largest timer gap=%llu ms, result=%s.\n",
                static_cast<unsigned long long>(g_app.heartbeat), static_cast<unsigned long long>(g_app.largestGap),
                g_app.failed ? "FAIL" : "PASS");
    return g_app.failed || message.wParam != 0 ? 1 : 0;
}