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
volatile LONG g_workerEntered = 0;
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

bool Checkpoint(bool automatic, const char* message) {
    std::printf("%s [%s; at most 2000 ms]\n", message,
                automatic ? "auto: continuing" : "Enter to continue");
    if (automatic) return true;
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input == nullptr || input == INVALID_HANDLE_VALUE || !GetConsoleMode(input, &mode)) {
        std::printf("No console input; continuing.\n");
        return true;
    }
    const ULONGLONG localDeadline = GetTickCount64() + 2000;
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

__declspec(noinline) DWORD WINAPI WorkerMain(void*) {
    InterlockedExchange(&g_workerEntered, 1);
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
        std::fprintf(stderr, "Usage: suspend_count.exe [--auto]\n");
        return 1;
    }
    g_deadline = GetTickCount64() + 18000;
    std::printf("PID=%lu (0x%lx), main TID=%lu (0x%lx)\n", GetCurrentProcessId(),
                GetCurrentProcessId(), GetCurrentThreadId(), GetCurrentThreadId());
    if (!NameThread(GetCurrentThread(), L"suspend-count-main")) return 1;
    g_enteredEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool success = Check(g_enteredEvent != nullptr, "CreateEventW(entered)");
    if (success) {
        g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        success = Check(g_stopEvent != nullptr, "CreateEventW(stop)");
    }
    HANDLE worker = nullptr;
    DWORD workerId = 0;
    unsigned resumesOwed = 0;
    if (success) {
        worker = CreateThread(nullptr, 0, WorkerMain, nullptr, CREATE_SUSPENDED, &workerId);
        success = Check(worker != nullptr, "CreateThread(CREATE_SUSPENDED)");
        if (success) {
            resumesOwed = 1;
            std::printf("Worker TID=%lu (0x%lx), HANDLE=%p; entered event=%p, stop event=%p\n",
                        workerId, workerId, worker, g_enteredEvent, g_stopEvent);
            success = NameThread(worker, L"suspend-count-worker");
        }
    }
    if (success) {
        const DWORD previous = SuspendThread(worker);
        if (previous == static_cast<DWORD>(-1)) {
            success = Check(FALSE, "SuspendThread(new, never-run thread)");
        } else {
            ++resumesOwed;
            std::printf("SuspendThread previous count=%lu (expected 1); our count is now 2.\n", previous);
            success = previous == 1 && InterlockedCompareExchange(&g_workerEntered, 0, 0) == 0;
            if (!success) std::fprintf(stderr, "Unexpected suspension count or early application entry.\n");
        }
    }
    if (success) success = Checkpoint(automatic, "Phase 1: count 2; application entry has never run. First resume next.");
    if (success) {
        const DWORD previous = ResumeThread(worker);
        if (previous == static_cast<DWORD>(-1)) {
            success = Check(FALSE, "ResumeThread(first)");
        } else {
            --resumesOwed;
            std::printf("First ResumeThread previous count=%lu (expected 2); one suspension remains.\n", previous);
            success = previous == 2;
        }
    }
    if (success) {
        const DWORD wait = WaitForSingleObject(g_enteredEvent, 250);
        const DWORD error = wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
        const LONG entered = InterlockedCompareExchange(&g_workerEntered, 0, 0);
        std::printf("After first resume: entry-event wait=0x%lx (expected WAIT_TIMEOUT=0x%lx), "
                    "entry flag=%ld (expected 0).\n", wait, static_cast<DWORD>(WAIT_TIMEOUT), entered);
        success = wait == WAIT_TIMEOUT && entered == 0;
        if (!success) std::fprintf(stderr, "No-entry check failed: error=%lu\n", error);
    }
    if (success) success = Checkpoint(automatic, "Phase 2: still suspended, not an application event wait. Final resume next.");
    if (success) {
        const DWORD previous = ResumeThread(worker);
        if (previous == static_cast<DWORD>(-1)) {
            success = Check(FALSE, "ResumeThread(second)");
        } else {
            --resumesOwed;
            std::printf("Second ResumeThread previous count=%lu (expected 1); final suspension removed.\n", previous);
            success = previous == 1;
        }
    }
    if (success) {
        success = Await(g_enteredEvent, "Actual worker entry");
        const LONG entered = InterlockedCompareExchange(&g_workerEntered, 0, 0);
        std::printf("Entry flag=%ld; signaled entry event proves application execution.\n", entered);
        success = success && entered == 1;
    }
    if (success) success = Checkpoint(automatic, "Phase 3: worker entered; now approaching/inside its cooperative event wait.");
    if (g_stopEvent != nullptr && !Check(SetEvent(g_stopEvent), "SetEvent(cooperative stop)")) success = false;
    while (worker != nullptr && resumesOwed != 0) {
        const DWORD previous = ResumeThread(worker);
        if (previous == static_cast<DWORD>(-1)) {
            Check(FALSE, "ResumeThread(cleanup)");
            success = false;
            break;
        }
        --resumesOwed;
        std::printf("Cleanup resume: previous count=%lu.\n", previous);
    }
    bool joined = worker == nullptr;
    if (worker != nullptr) {
        joined = Await(worker, "Worker join");
        if (!joined) success = false;
        if (joined) {
            DWORD exitCode = 0;
            if (!Check(GetExitCodeThread(worker, &exitCode), "GetExitCodeThread")) success = false;
            const DWORD error = static_cast<DWORD>(InterlockedCompareExchange(&g_workerError, 0, 0));
            std::printf("Worker exit=%lu (expected 42), worker error=%lu.\n", exitCode, error);
            if (exitCode != 42 || error != ERROR_SUCCESS) success = false;
        }
        if (!Check(CloseHandle(worker), "CloseHandle(thread)")) success = false;
    }
    if (joined) {
        if (g_enteredEvent != nullptr && !Check(CloseHandle(g_enteredEvent), "CloseHandle(entered)")) success = false;
        if (g_stopEvent != nullptr && !Check(CloseHandle(g_stopEvent), "CloseHandle(stop)")) success = false;
    }
    std::printf("%s: suspend count and actual entry were checked independently.\n", success ? "PASS" : "FAIL");
    return success ? 0 : 1;
}