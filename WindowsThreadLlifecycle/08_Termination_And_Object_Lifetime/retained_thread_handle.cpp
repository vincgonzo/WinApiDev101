#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>

HANDLE g_enteredEvent = nullptr;
HANDLE g_stopEvent = nullptr;
volatile LONG g_workerError = 0;
ULONGLONG g_deadline = 0;

DWORD RemainingMs() {
    const ULONGLONG now = GetTickCount64();
    return now < g_deadline ? static_cast<DWORD>(g_deadline - now) : 1;
}

bool Check(BOOL result, const char* operation) {
    if (result) return true;
    std::fprintf(stderr, "%s failed: Win32 error %lu\n", operation, GetLastError());
    return false;
}

bool NameThread(HANDLE thread, const wchar_t* name) {
    using DescriptionFunction = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    HMODULE module = GetModuleHandleW(L"KernelBase.dll");
    if (!Check(module != nullptr, "GetModuleHandleW(KernelBase)")) return false;
    const FARPROC address = GetProcAddress(module, "SetThreadDescription");
    if (!Check(address != nullptr, "GetProcAddress(SetThreadDescription)")) return false;
    DescriptionFunction describeThread = nullptr;
    static_assert(sizeof(describeThread) == sizeof(address), "Function pointer size mismatch");
    std::memcpy(&describeThread, &address, sizeof(describeThread));
    const HRESULT result = describeThread(thread, name);
    if (SUCCEEDED(result)) return true;
    std::fprintf(stderr, "SetThreadDescription failed: HRESULT 0x%08lx\n",
                 static_cast<unsigned long>(result));
    return false;
}

bool Await(HANDLE handle, const char* operation) {
    const DWORD wait = WaitForSingleObject(handle, RemainingMs());
    if (wait == WAIT_OBJECT_0) return true;
    std::fprintf(stderr, "%s: wait=0x%lx, error=%lu\n", operation, wait,
                 wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
    return false;
}

bool Checkpoint(bool automatic, const char* message, DWORD durationMs) {
    std::printf("%s [%s; at most %lu ms]\n", message,
                automatic ? "auto: continuing" : "Enter to continue", durationMs);
    if (automatic) return true;
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input == nullptr || input == INVALID_HANDLE_VALUE || !GetConsoleMode(input, &mode)) {
        std::printf("No console input; continuing.\n");
        return true;
    }
    const ULONGLONG localDeadline = GetTickCount64() + durationMs;
    const ULONGLONG deadline = localDeadline < g_deadline ? localDeadline : g_deadline;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return true;
        const DWORD wait = WaitForSingleObject(input, static_cast<DWORD>(deadline - now));
        if (wait == WAIT_TIMEOUT) return true;
        if (wait != WAIT_OBJECT_0) return Check(FALSE, "WaitForSingleObject(console)");
        INPUT_RECORD record{};
        DWORD read = 0;
        if (!Check(ReadConsoleInputW(input, &record, 1, &read), "ReadConsoleInputW")) return false;
        if (read == 1 && record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown &&
            record.Event.KeyEvent.wVirtualKeyCode == VK_RETURN) return true;
    }
}

__declspec(noinline) DWORD WINAPI CooperativeWorker(void*) {
    if (!SetEvent(g_enteredEvent)) {
        InterlockedExchange(&g_workerError, static_cast<LONG>(GetLastError()));
        return 125;
    }
    const DWORD wait = WaitForSingleObject(g_stopEvent, RemainingMs());
    if (wait == WAIT_OBJECT_0) return 42;
    InterlockedExchange(&g_workerError,
                        static_cast<LONG>(wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT));
    return 124;
}

int main(int argumentCount, char** arguments) {
    const bool automatic = argumentCount == 2 && std::strcmp(arguments[1], "--auto") == 0;
    if (argumentCount > 1 && !automatic) {
        std::fprintf(stderr, "Usage: retained_thread_handle.exe [--auto]\n");
        return 1;
    }
    g_deadline = GetTickCount64() + 18000;
    std::printf("PID=%lu (0x%lx), main TID=%lu (0x%lx)\n", GetCurrentProcessId(),
                GetCurrentProcessId(), GetCurrentThreadId(), GetCurrentThreadId());
    if (!NameThread(GetCurrentThread(), L"retained-handle-main")) return 1;
    g_enteredEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool success = Check(g_enteredEvent != nullptr, "CreateEventW(entered)");
    if (success) {
        g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        success = Check(g_stopEvent != nullptr, "CreateEventW(stop)");
    }
    DWORD workerId = 0;
    HANDLE worker = nullptr;
    if (success) {
        worker = CreateThread(nullptr, 0, CooperativeWorker, nullptr, 0, &workerId);
        success = Check(worker != nullptr, "CreateThread");
        if (success) {
            std::printf("Worker TID=%lu (0x%lx), retained thread HANDLE=%p, stop event=%p\n",
                        workerId, workerId, worker, g_stopEvent);
            success = NameThread(worker, L"retained-handle-worker") && Await(g_enteredEvent, "Worker entry");
        }
    }
    if (success) {
        DWORD exitCode = 0;
        success = Check(GetExitCodeThread(worker, &exitCode), "GetExitCodeThread(before stop)");
        std::printf("Phase 1: entered worker, exit-status query=%lu (STILL_ACTIVE=%lu).\n",
                    exitCode, static_cast<DWORD>(STILL_ACTIVE));
        if (exitCode != STILL_ACTIVE) success = false;
    }
    if (success) success = Checkpoint(automatic, "Inspect the live worker's cooperative event wait; request stop next.", 3000);
    if (g_stopEvent != nullptr) {
        std::printf("Phase 2: signal cooperative stop, then join before interpreting exit status.\n");
        if (!Check(SetEvent(g_stopEvent), "SetEvent(stop)")) success = false;
    }
    bool joined = worker == nullptr;
    if (worker != nullptr) {
        joined = Await(worker, "Worker join");
        if (!joined) success = false;
        if (joined) {
            DWORD exitCode = 0;
            if (!Check(GetExitCodeThread(worker, &exitCode), "GetExitCodeThread(after join)")) success = false;
            const DWORD error = static_cast<DWORD>(InterlockedCompareExchange(&g_workerError, 0, 0));
            std::printf("Thread signaled: execution ended; TID=%lu (0x%lx), HANDLE=%p, exit=%lu (expected 42), error=%lu.\n",
                        workerId, workerId, worker, exitCode, error);
            if (exitCode != 42 || error != ERROR_SUCCESS) success = false;
        }
    }
    if (success) success = Checkpoint(automatic, "Phase 3: completed thread HANDLE retained. Inspect object/exit status before close.", 6000);
    if (success) {
        const DWORD wait = WaitForSingleObject(worker, 0);
        if (wait != WAIT_OBJECT_0) {
            std::fprintf(stderr, "Retained thread should remain signaled: wait=0x%lx, error=%lu\n", wait,
                         wait == WAIT_FAILED ? GetLastError() : ERROR_INVALID_DATA);
            success = false;
        }
        DWORD exitCode = 0;
        if (!Check(GetExitCodeThread(worker, &exitCode), "GetExitCodeThread(retained handle)")) success = false;
        std::printf("Retained-handle query still returns %lu. Closing our reference next.\n", exitCode);
        if (exitCode != 42) success = false;
    }
    if (worker != nullptr) {
        if (!Check(CloseHandle(worker), "CloseHandle(thread)")) success = false;
        worker = nullptr;
    }
    if (joined) {
        if (g_enteredEvent != nullptr && !Check(CloseHandle(g_enteredEvent), "CloseHandle(entered)")) success = false;
        if (g_stopEvent != nullptr && !Check(CloseHandle(g_stopEvent), "CloseHandle(stop)")) success = false;
    }
    std::printf("%s: our handle closed; final object deletion is not proved (other references may exist).\n",
                success ? "PASS" : "FAIL");
    return success ? 0 : 1;
}