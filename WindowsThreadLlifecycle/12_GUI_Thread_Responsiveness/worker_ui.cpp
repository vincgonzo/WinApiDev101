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
#include <process.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

constexpr int StartControlId = 100;
constexpr UINT_PTR HeartbeatTimer = 1;
constexpr UINT WorkCompletedMessage = WM_APP + 1;
constexpr wchar_t WindowClassName[] = L"ThreadTrainingWorkerWindow";
using DescriptionFunction = HRESULT(WINAPI*)(HANDLE, PCWSTR);
DescriptionFunction g_describeThread = nullptr;

struct ComputationResult {
    ULONGLONG checksum;
    ULONGLONG batches;
    ULONGLONG elapsedMilliseconds;
    bool cancelled;
};

struct WorkContext {
    HWND window;
    DWORD durationMilliseconds;
    DWORD runNumber;
    LONG cancelRequested;
    DWORD actualThreadId;
    HRESULT nameResult;
    DWORD postError;
    ComputationResult result;
};

struct AppState {
    HINSTANCE instance;
    HWND window;
    HWND startButton;
    HWND identityText;
    HWND statusText;
    HWND timingText;
    HWND resultText;
    HANDLE worker;
    bool automatic;
    bool busy;
    bool completed;
    bool closing;
    bool failed;
    bool timerActive;
    bool completionMessageReceived;
    DWORD uiThreadId;
    DWORD workerThreadId;
    DWORD runNumber;
    ULONGLONG appStarted;
    ULONGLONG workStarted;
    ULONGLONG heartbeat;
    ULONGLONG heartbeatAtStart;
    ULONGLONG workHeartbeats;
    ULONGLONG lastHeartbeat;
    ULONGLONG largestGap;
    WorkContext work;
    ComputationResult result;
    const wchar_t* status;
};

AppState g_app{};

void RecordFailure(const char* operation, DWORD error) {
    std::fprintf(stderr, "%s failed: error %lu (0x%lx)\n", operation, error, error);
    g_app.failed = true;
}

bool Check(BOOL success, const char* operation) {
    if (!success) RecordFailure(operation, GetLastError());
    return success != FALSE;
}

[[noreturn]] void FatalFailure(const char* operation, DWORD error) {
    RecordFailure(operation, error);
    ExitProcess(1);
}

bool InitializeThreadNaming() {
    HMODULE module = GetModuleHandleW(L"KernelBase.dll");
    if (!Check(module != nullptr, "GetModuleHandleW(KernelBase)")) return false;
    const FARPROC address = GetProcAddress(module, "SetThreadDescription");
    if (!Check(address != nullptr, "GetProcAddress(SetThreadDescription)")) return false;
    static_assert(sizeof(g_describeThread) == sizeof(address), "Function pointer sizes must match");
    std::memcpy(&g_describeThread, &address, sizeof(g_describeThread));
    const HRESULT result = g_describeThread(GetCurrentThread(), L"worker-ui");
    if (FAILED(result)) RecordFailure("SetThreadDescription(UI)", static_cast<DWORD>(result));
    return SUCCEEDED(result);
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

__declspec(noinline) unsigned __stdcall ComputeWorker(void* parameter) {
    auto* context = static_cast<WorkContext*>(parameter);
    context->actualThreadId = GetCurrentThreadId();
    context->nameResult = g_describeThread(GetCurrentThread(), L"responsive-worker");
    if (SUCCEEDED(context->nameResult)) {
        context->result = ComputeFor(context->durationMilliseconds, &context->cancelRequested);
    }
    if (!PostMessageW(context->window, WorkCompletedMessage, context->runNumber, 0)) {
        const DWORD error = GetLastError();
        context->postError = error != ERROR_SUCCESS ? error : ERROR_GEN_FAILURE;
    }
    return FAILED(context->nameResult) || context->postError != ERROR_SUCCESS ? 1U : 0U;
}

bool SetText(HWND control, const wchar_t* text) {
    return Check(SetWindowTextW(control, text), "SetWindowTextW");
}

bool RefreshDisplay() {
    if (g_app.window == nullptr || g_app.identityText == nullptr) return true;
    wchar_t buffer[256]{};
    int length = std::swprintf(buffer, 256, L"PID: %lu (0x%lx)\nUI TID: %lu (0x%lx)\nWorker TID: %lu (0x%lx)",
                              GetCurrentProcessId(), GetCurrentProcessId(), g_app.uiThreadId, g_app.uiThreadId,
                              g_app.workerThreadId, g_app.workerThreadId);
    if (length < 0) {
        RecordFailure("Format identity", ERROR_INSUFFICIENT_BUFFER);
        return false;
    }
    if (!SetText(g_app.identityText, buffer) || !SetText(g_app.statusText, g_app.status)) return false;
    const ULONGLONG elapsed = g_app.busy ? GetTickCount64() - g_app.workStarted : g_app.result.elapsedMilliseconds;
    length = std::swprintf(buffer, 256, L"Elapsed: %llu ms\nHeartbeat: %llu\nLargest timer gap: %llu ms",
                          static_cast<unsigned long long>(elapsed), static_cast<unsigned long long>(g_app.heartbeat),
                          static_cast<unsigned long long>(g_app.largestGap));
    if (length < 0) {
        RecordFailure("Format timing", ERROR_INSUFFICIENT_BUFFER);
        return false;
    }
    if (!SetText(g_app.timingText, buffer)) return false;
    const ULONGLONG workHeartbeats = g_app.busy ? g_app.heartbeat - g_app.heartbeatAtStart : g_app.workHeartbeats;
    length = std::swprintf(buffer, 256, L"Checksum: %llu\nBatches: %llu\nHeartbeats during work: %llu",
                          static_cast<unsigned long long>(g_app.result.checksum),
                          static_cast<unsigned long long>(g_app.result.batches),
                          static_cast<unsigned long long>(workHeartbeats));
    if (length < 0) {
        RecordFailure("Format result", ERROR_INSUFFICIENT_BUFFER);
        return false;
    }
    return SetText(g_app.resultText, buffer);
}

bool LayoutControls(HWND window) {
    if (g_app.startButton == nullptr) return true;
    RECT client{};
    if (!Check(GetClientRect(window, &client), "GetClientRect")) return false;
    const int width = client.right > 32 ? static_cast<int>(client.right) - 32 : 1;
    return Check(MoveWindow(g_app.startButton, 16, 16, 112, 32, TRUE), "MoveWindow(Start)")
           && Check(MoveWindow(g_app.identityText, 16, 62, width, 60, TRUE), "MoveWindow(identity)")
           && Check(MoveWindow(g_app.statusText, 16, 130, width, 40, TRUE), "MoveWindow(status)")
           && Check(MoveWindow(g_app.timingText, 16, 180, width, 60, TRUE), "MoveWindow(timing)")
           && Check(MoveWindow(g_app.resultText, 16, 250, width, 60, TRUE), "MoveWindow(result)");
}

HWND CreateLabel(HWND parent) {
    HWND control = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   0, 0, 1, 1, parent, nullptr, g_app.instance, nullptr);
    Check(control != nullptr, "CreateWindowExW(STATIC)");
    return control;
}

void BeginClose() {
    g_app.closing = true;
    if (g_app.startButton != nullptr) EnableWindow(g_app.startButton, FALSE);
    if (g_app.worker != nullptr) {
        InterlockedExchange(&g_app.work.cancelRequested, 1);
        g_app.status = L"Closing: cancellation requested";
        RefreshDisplay();
        return;
    }
    if (g_app.window != nullptr && !DestroyWindow(g_app.window)) {
        FatalFailure("DestroyWindow", GetLastError());
    }
}

void TryFinishWorker() {
    if (g_app.worker == nullptr) return;
    const DWORD waitResult = WaitForSingleObject(g_app.worker, 0);
    if (waitResult == WAIT_TIMEOUT) return;
    if (waitResult != WAIT_OBJECT_0) FatalFailure("Check worker completion", GetLastError());
    if (g_app.work.postError == ERROR_SUCCESS && !g_app.completionMessageReceived) return;

    DWORD exitCode = 1;
    Check(GetExitCodeThread(g_app.worker, &exitCode), "GetExitCodeThread");
    Check(CloseHandle(g_app.worker), "CloseHandle(worker)");
    g_app.worker = nullptr;
    g_app.busy = false;
    g_app.result = g_app.work.result;
    g_app.workHeartbeats = g_app.heartbeat - g_app.heartbeatAtStart;
    if (FAILED(g_app.work.nameResult)) RecordFailure("SetThreadDescription(worker)", static_cast<DWORD>(g_app.work.nameResult));
    if (g_app.work.postError != ERROR_SUCCESS) RecordFailure("PostMessageW(worker completion)", g_app.work.postError);
    if (exitCode != 0) RecordFailure("Worker exit code", exitCode);
    if (g_app.work.actualThreadId != g_app.workerThreadId || g_app.workerThreadId == g_app.uiThreadId) {
        RecordFailure("Worker thread identity", ERROR_INVALID_DATA);
    }
    if (!g_app.result.cancelled && (g_app.result.batches == 0 || g_app.result.checksum == 0)) {
        RecordFailure("Empty computation result", ERROR_INVALID_DATA);
    }
    if (g_app.automatic && (g_app.result.cancelled || g_app.workHeartbeats == 0)) {
        RecordFailure("Automatic completion or heartbeat check", ERROR_INVALID_DATA);
    }
    g_app.completed = !g_app.failed && !g_app.result.cancelled;
    g_app.status = g_app.failed ? L"Failed" : (g_app.result.cancelled ? L"Cancelled" : L"Completed on the worker");
    std::printf("Phase 3: worker joined; TID=%lu (0x%lx), elapsed=%llu ms, checksum=%llu, batches=%llu, heartbeats=%llu, cancelled=%s.\n",
                g_app.workerThreadId, g_app.workerThreadId,
                static_cast<unsigned long long>(g_app.result.elapsedMilliseconds),
                static_cast<unsigned long long>(g_app.result.checksum),
                static_cast<unsigned long long>(g_app.result.batches),
                static_cast<unsigned long long>(g_app.workHeartbeats), g_app.result.cancelled ? "yes" : "no");
    std::printf("Completion notification: %s.\n",
                g_app.completionMessageReceived ? "WM_APP message" : "thread-handle fallback after PostMessage failure");
    RefreshDisplay();
    if (g_app.closing || g_app.automatic || g_app.failed) {
        BeginClose();
    } else {
        EnableWindow(g_app.startButton, TRUE);
    }
}

void StartWork() {
    if (g_app.busy || g_app.closing || g_app.failed) return;
    ++g_app.runNumber;
    g_app.work = WorkContext{};
    g_app.work.window = g_app.window;
    g_app.work.durationMilliseconds = g_app.automatic ? 1000 : 4000;
    g_app.work.runNumber = g_app.runNumber;
    g_app.result = ComputationResult{};
    g_app.completionMessageReceived = false;
    g_app.completed = false;
    g_app.busy = true;
    g_app.heartbeatAtStart = g_app.heartbeat;
    g_app.workStarted = GetTickCount64();
    g_app.status = L"Computing on the worker";
    EnableWindow(g_app.startButton, FALSE);
    unsigned workerId = 0;
    const uintptr_t rawHandle = _beginthreadex(nullptr, 0, ComputeWorker, &g_app.work, 0, &workerId);
    if (rawHandle == 0) {
        std::fprintf(stderr, "_beginthreadex failed: CRT errno=%d.\n", errno);
        g_app.failed = true;
        g_app.busy = false;
        BeginClose();
        return;
    }
    g_app.worker = reinterpret_cast<HANDLE>(rawHandle);
    g_app.workerThreadId = static_cast<DWORD>(workerId);
    std::printf("Phase 2: started run %lu, UI TID=%lu (0x%lx), worker TID=%lu (0x%lx).\n",
                g_app.runNumber, g_app.uiThreadId, g_app.uiThreadId, g_app.workerThreadId, g_app.workerThreadId);
    RefreshDisplay();
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wordParameter, LPARAM longParameter) {
    switch (message) {
    case WM_CREATE:
        g_app.window = window;
        g_app.startButton = CreateWindowExW(0, L"BUTTON", L"&Start", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                           16, 16, 112, 32, window,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(StartControlId)), g_app.instance, nullptr);
        if (!Check(g_app.startButton != nullptr, "CreateWindowExW(Start)")) return -1;
        g_app.identityText = CreateLabel(window);
        g_app.statusText = CreateLabel(window);
        g_app.timingText = CreateLabel(window);
        g_app.resultText = CreateLabel(window);
        if (g_app.failed || !LayoutControls(window) || !RefreshDisplay()) return -1;
        if (!Check(SetTimer(window, HeartbeatTimer, 100, nullptr) != 0, "SetTimer")) return -1;
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
    case WorkCompletedMessage:
        if (g_app.busy && wordParameter == g_app.work.runNumber) {
            g_app.completionMessageReceived = true;
            TryFinishWorker();
        }
        return 0;
    case WM_TIMER:
        if (wordParameter == HeartbeatTimer) {
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG gap = now - g_app.lastHeartbeat;
            if (gap > g_app.largestGap) g_app.largestGap = gap;
            g_app.lastHeartbeat = now;
            ++g_app.heartbeat;
            RefreshDisplay();
            return 0;
        }
        break;
    case WM_CLOSE:
        if (g_app.automatic && !g_app.completed) RecordFailure("Early automatic close", ERROR_CANCELLED);
        BeginClose();
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
        std::fprintf(stderr, "Usage: worker_ui.exe [--auto]\n");
        return 1;
    }
    if (!InitializeThreadNaming()) return 1;
    g_app.instance = GetModuleHandleW(nullptr);
    if (!Check(g_app.instance != nullptr, "GetModuleHandleW")) return 1;
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
    if (!Check(windowClass.hCursor != nullptr, "LoadCursorW")) return 1;
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
    windowClass.lpszClassName = WindowClassName;
    if (!Check(RegisterClassW(&windowClass) != 0, "RegisterClassW")) return 1;
    HWND window = CreateWindowExW(0, WindowClassName, L"Responsive UI with worker", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 720, 420, nullptr, nullptr, g_app.instance, nullptr);
    if (!Check(window != nullptr, "CreateWindowExW(main)")) {
        Check(UnregisterClassW(WindowClassName, g_app.instance), "UnregisterClassW");
        return 1;
    }
    ShowWindow(window, SW_SHOWNORMAL);
    Check(UpdateWindow(window), "UpdateWindow");
    if (g_app.automatic) {
        Check(PostMessageW(window, WM_COMMAND, MAKEWPARAM(StartControlId, BN_CLICKED),
                           reinterpret_cast<LPARAM>(g_app.startButton)), "PostMessageW(Start)");
    }

    bool quit = false;
    while (!quit) {
        HANDLE watchedWorker = g_app.worker;
        const DWORD handleCount = watchedWorker != nullptr ? 1 : 0;
        const DWORD waitResult = MsgWaitForMultipleObjectsEx(handleCount, handleCount != 0 ? &watchedWorker : nullptr,
                                                            50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (waitResult == WAIT_FAILED) FatalFailure("MsgWaitForMultipleObjectsEx", GetLastError());
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                if (g_app.worker != nullptr) {
                    RecordFailure("Quit before worker join", ERROR_INVALID_DATA);
                    BeginClose();
                } else {
                    if (message.wParam != 0) g_app.failed = true;
                    quit = true;
                }
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (g_app.automatic && !g_app.closing && GetTickCount64() - g_app.appStarted > 12000) {
            RecordFailure("Automatic UI watchdog", ERROR_TIMEOUT);
        }
        if (g_app.failed && !g_app.closing) BeginClose();
        TryFinishWorker();
        if (g_app.window == nullptr && g_app.worker == nullptr) quit = true;
    }
    if (g_app.window != nullptr) BeginClose();
    if (g_app.automatic && !g_app.completed) g_app.failed = true;
    Check(UnregisterClassW(WindowClassName, g_app.instance), "UnregisterClassW");
    std::printf("Phase 4: worker handle closed before window destruction; heartbeat=%llu, largest gap=%llu ms, result=%s.\n",
                static_cast<unsigned long long>(g_app.heartbeat), static_cast<unsigned long long>(g_app.largestGap),
                g_app.failed ? "FAIL" : "PASS");
    return g_app.failed ? 1 : 0;
}