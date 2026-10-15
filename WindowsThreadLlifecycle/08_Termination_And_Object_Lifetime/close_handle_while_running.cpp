#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <process.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

HANDLE g_enteredEvent = nullptr;
HANDLE g_stopEvent = nullptr;
HANDLE g_afterCloseRequest = nullptr;
HANDLE g_afterCloseObserved = nullptr;
ULONGLONG g_deadline = 0;
std::atomic<unsigned long long> g_progress{0};
std::atomic<unsigned long long> g_acknowledgedProgress{0};
std::atomic<DWORD> g_workerError{ERROR_SUCCESS};

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
    std::printf("%s [%s; at most 3000 ms]\n", message,
                automatic ? "auto: continuing" : "Enter to continue");
    if (automatic) return true;
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input == nullptr || input == INVALID_HANDLE_VALUE || !GetConsoleMode(input, &mode)) {
        std::printf("No console input; continuing.\n");
        return true;
    }
    const ULONGLONG localDeadline = GetTickCount64() + 3000;
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

unsigned WorkerFailure(DWORD error) {
    g_workerError.store(error);
    return 1;
}

__declspec(noinline) unsigned __stdcall ProgressWorker(void*) {
    g_progress.fetch_add(1);
    if (!SetEvent(g_enteredEvent)) return WorkerFailure(GetLastError());
    const HANDLE stopOrRequest[] = {g_stopEvent, g_afterCloseRequest};
    bool acknowledged = false;
    while (GetTickCount64() < g_deadline) {
        const DWORD remaining = RemainingMs();
        const DWORD interval = remaining < 100 ? remaining : 100;
        const DWORD wait = acknowledged ? WaitForSingleObject(g_stopEvent, interval) :
                          WaitForMultipleObjects(2, stopOrRequest, FALSE, interval);
        if (wait == WAIT_OBJECT_0) return 42;
        if (wait == WAIT_TIMEOUT) {
            g_progress.fetch_add(1);
            continue;
        }
        if (!acknowledged && wait == WAIT_OBJECT_0 + 1) {
            const unsigned long long progress = g_progress.fetch_add(1) + 1;
            g_acknowledgedProgress.store(progress);
            if (!SetEvent(g_afterCloseObserved)) return WorkerFailure(GetLastError());
            acknowledged = true;
            continue;
        }
        return WorkerFailure(wait == WAIT_FAILED ? GetLastError() : ERROR_INVALID_DATA);
    }
    return WorkerFailure(ERROR_TIMEOUT);
}

int main(int argumentCount, char** arguments) {
    const bool automatic = argumentCount == 2 && std::strcmp(arguments[1], "--auto") == 0;
    if (argumentCount > 1 && !automatic) {
        std::fprintf(stderr, "Usage: close_handle_while_running.exe [--auto]\n");
        return 1;
    }
    g_deadline = GetTickCount64() + 18000;
    std::printf("PID=%lu (0x%lx), main TID=%lu (0x%lx)\n", GetCurrentProcessId(),
                GetCurrentProcessId(), GetCurrentThreadId(), GetCurrentThreadId());
    if (!NameThread(GetCurrentThread(), L"close-handle-main")) return 1;
    HANDLE* events[] = {&g_enteredEvent, &g_stopEvent, &g_afterCloseRequest, &g_afterCloseObserved};
    bool success = true;
    for (HANDLE* event : events) {
        *event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!Check(*event != nullptr, "CreateEventW")) {
            success = false;
            break;
        }
    }
    HANDLE original = nullptr;
    HANDLE duplicate = nullptr;
    unsigned workerId = 0;
    if (success) {
        original = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, ProgressWorker, nullptr, 0, &workerId));
        if (original == nullptr) {
            std::fprintf(stderr, "_beginthreadex failed: errno %d\n", errno);
            success = false;
        } else {
            std::printf("Worker TID=%u (0x%x), original HANDLE=%p\n", workerId, workerId, original);
            success = NameThread(original, L"close-handle-worker") &&
                      Check(DuplicateHandle(GetCurrentProcess(), original, GetCurrentProcess(),
                                            &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS), "DuplicateHandle") &&
                      Await(g_enteredEvent, "Worker entry");
        }
    }
    if (success) {
        std::printf("Phase 1: original=%p, duplicate=%p reference the SAME thread; progress=%llu.\n",
                    original, duplicate, g_progress.load());
        std::printf("Duplicate retained for join: this is NOT a last-handle-removal experiment.\n");
        success = Checkpoint(automatic, "Inspect two handles and live worker. Close only original next.");
    }
    unsigned long long beforeClose = 0;
    if (success) {
        beforeClose = g_progress.load();
        if (Check(CloseHandle(original), "CloseHandle(original while running)")) {
            original = nullptr;
            std::printf("Phase 2: original closed; TID=%u (0x%x) unchanged; duplicate=%p remains valid.\n",
                        workerId, workerId, duplicate);
            success = Check(SetEvent(g_afterCloseRequest), "SetEvent(post-close request)") &&
                      Await(g_afterCloseObserved, "Post-close worker acknowledgment");
        } else {
            success = false;
        }
    }
    if (success) {
        const unsigned long long acknowledged = g_acknowledgedProgress.load();
        std::printf("Progress before close=%llu; causally post-close acknowledged progress=%llu; current=%llu.\n",
                    beforeClose, acknowledged, g_progress.load());
        if (acknowledged <= beforeClose) {
            std::fprintf(stderr, "Post-close request must cause a new progress increment.\n");
            success = false;
        }
    }
    if (success) success = Checkpoint(automatic, "Phase 3: worker still live with duplicate handle. Cooperative stop next.");
    if (g_stopEvent != nullptr && !Check(SetEvent(g_stopEvent), "SetEvent(cooperative stop)")) success = false;
    const HANDLE joinHandle = duplicate != nullptr ? duplicate : original;
    bool joined = joinHandle == nullptr;
    if (joinHandle != nullptr) {
        joined = Await(joinHandle, "Worker join through retained handle");
        if (!joined) success = false;
        if (joined) {
            DWORD exitCode = 0;
            if (!Check(GetExitCodeThread(joinHandle, &exitCode), "GetExitCodeThread")) success = false;
            std::printf("Joined using %s; worker exit=%lu (expected 42), progress=%llu, worker error=%lu.\n",
                        duplicate != nullptr ? "duplicate" : "original (setup-error cleanup)", exitCode,
                        g_progress.load(), g_workerError.load());
            if (exitCode != 42 || g_workerError.load() != ERROR_SUCCESS) success = false;
        }
    }
    if (duplicate != nullptr) {
        if (!Check(CloseHandle(duplicate), "CloseHandle(duplicate)")) success = false;
        duplicate = nullptr;
    }
    if (original != nullptr) {
        if (!Check(CloseHandle(original), "CloseHandle(original cleanup)")) success = false;
        original = nullptr;
    }
    if (joined) {
        for (HANDLE* event : events) if (*event != nullptr && !Check(CloseHandle(*event), "CloseHandle(event)")) success = false;
    }
    std::printf("%s: closing a handle did not stop execution; stop event + join ended it cooperatively.\n",
                success ? "PASS" : "FAIL");
    return success ? 0 : 1;
}