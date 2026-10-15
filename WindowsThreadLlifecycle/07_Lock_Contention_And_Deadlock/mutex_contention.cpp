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

struct WorkerState {
    std::atomic<DWORD> error{ERROR_SUCCESS};
    std::atomic<bool> abandoned{false};
    std::atomic<bool> acquired{false};
};

HANDLE g_mutex = nullptr;
HANDLE g_ownerOwns = nullptr;
HANDLE g_contenderReady = nullptr;
HANDLE g_contentionObserved = nullptr;
HANDLE g_releaseOwner = nullptr;
HANDLE g_abort = nullptr;
ULONGLONG g_deadline = 0;
WorkerState g_owner;
WorkerState g_contender;
unsigned g_protectedValue = 0;

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

bool Checkpoint(bool automatic) {
    std::printf("Phase 2: owner holds mutex; contender's 100 ms acquisition timed out.\n");
    std::printf("Contender now approaches/enters its second wait. Inspect, then release owner [%s; max 4000 ms].\n",
                automatic ? "auto: continuing" : "Enter to continue");
    if (automatic) return true;
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input == nullptr || input == INVALID_HANDLE_VALUE || !GetConsoleMode(input, &mode)) {
        std::printf("No console input; continuing.\n");
        return true;
    }
    const ULONGLONG localDeadline = GetTickCount64() + 4000;
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

unsigned WorkerFailure(WorkerState& state, DWORD error) {
    state.error.store(error);
    if (!SetEvent(g_abort)) state.error.store(GetLastError());
    return 1;
}

unsigned ReleaseAndFail(WorkerState& state, DWORD error) {
    if (!ReleaseMutex(g_mutex)) error = GetLastError();
    return WorkerFailure(state, error);
}

__declspec(noinline) unsigned __stdcall OwnerMain(void*) {
    const DWORD acquired = WaitForSingleObject(g_mutex, RemainingMs());
    if (acquired == WAIT_ABANDONED) {
        g_owner.abandoned.store(true);
        return ReleaseAndFail(g_owner, ERROR_ABANDONED_WAIT_0);
    }
    if (acquired != WAIT_OBJECT_0)
        return WorkerFailure(g_owner, acquired == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
    g_owner.acquired.store(true);
    g_protectedValue = 1;
    if (!SetEvent(g_ownerOwns)) return ReleaseAndFail(g_owner, GetLastError());
    const HANDLE releaseOrAbort[] = {g_abort, g_releaseOwner};
    const DWORD released = WaitForMultipleObjects(2, releaseOrAbort, FALSE, RemainingMs());
    if (released != WAIT_OBJECT_0 + 1) {
        const DWORD error = released == WAIT_FAILED ? GetLastError() :
                            (released == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED);
        return ReleaseAndFail(g_owner, error);
    }
    g_protectedValue = 2;
    if (!ReleaseMutex(g_mutex)) return WorkerFailure(g_owner, GetLastError());
    return 0;
}

__declspec(noinline) unsigned __stdcall ContenderMain(void*) {
    const HANDLE ownedOrAbort[] = {g_abort, g_ownerOwns};
    DWORD wait = WaitForMultipleObjects(2, ownedOrAbort, FALSE, RemainingMs());
    if (wait != WAIT_OBJECT_0 + 1)
        return WorkerFailure(g_contender, wait == WAIT_FAILED ? GetLastError() :
                             (wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED));
    if (!SetEvent(g_contenderReady)) return WorkerFailure(g_contender, GetLastError());
    const HANDLE mutexOrAbort[] = {g_abort, g_mutex};
    wait = WaitForMultipleObjects(2, mutexOrAbort, FALSE, 100);
    if (wait == WAIT_ABANDONED_0 + 1) {
        g_contender.abandoned.store(true);
        return ReleaseAndFail(g_contender, ERROR_ABANDONED_WAIT_0);
    }
    if (wait == WAIT_OBJECT_0 + 1) return ReleaseAndFail(g_contender, ERROR_INVALID_DATA);
    if (wait != WAIT_TIMEOUT)
        return WorkerFailure(g_contender, wait == WAIT_FAILED ? GetLastError() : ERROR_OPERATION_ABORTED);
    if (!SetEvent(g_contentionObserved)) return WorkerFailure(g_contender, GetLastError());
    wait = WaitForMultipleObjects(2, mutexOrAbort, FALSE, RemainingMs());
    if (wait == WAIT_ABANDONED_0 + 1) {
        g_contender.abandoned.store(true);
        return ReleaseAndFail(g_contender, ERROR_ABANDONED_WAIT_0);
    }
    if (wait != WAIT_OBJECT_0 + 1)
        return WorkerFailure(g_contender, wait == WAIT_FAILED ? GetLastError() :
                             (wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED));
    g_contender.acquired.store(true);
    if (g_protectedValue != 2) return ReleaseAndFail(g_contender, ERROR_INVALID_DATA);
    g_protectedValue = 3;
    if (!ReleaseMutex(g_mutex)) return WorkerFailure(g_contender, GetLastError());
    return 0;
}

bool AwaitPhase(HANDLE phase, const char* operation) {
    const HANDLE handles[] = {g_abort, phase};
    const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, RemainingMs());
    if (wait == WAIT_OBJECT_0 + 1) return true;
    std::fprintf(stderr, "%s failed/aborted: wait=0x%lx, error=%lu\n", operation, wait,
                 wait == WAIT_FAILED ? GetLastError() :
                 (wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED));
    return false;
}

int main(int argumentCount, char** arguments) {
    const bool automatic = argumentCount == 2 && std::strcmp(arguments[1], "--auto") == 0;
    if (argumentCount > 1 && !automatic) {
        std::fprintf(stderr, "Usage: mutex_contention.exe [--auto]\n");
        return 1;
    }
    g_deadline = GetTickCount64() + 18000;
    std::printf("PID=%lu (0x%lx), main TID=%lu (0x%lx)\n", GetCurrentProcessId(),
                GetCurrentProcessId(), GetCurrentThreadId(), GetCurrentThreadId());
    if (!NameThread(GetCurrentThread(), L"mutex-main")) return 1;
    HANDLE* events[] = {&g_ownerOwns, &g_contenderReady, &g_contentionObserved, &g_releaseOwner, &g_abort};
    bool success = true;
    for (HANDLE* event : events) {
        *event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!Check(*event != nullptr, "CreateEventW")) {
            success = false;
            break;
        }
    }
    if (success) {
        g_mutex = CreateMutexW(nullptr, FALSE, nullptr);
        success = Check(g_mutex != nullptr, "CreateMutexW");
    }
    std::printf("Mutex HANDLE=%p; owner-owns=%p; contender-ready=%p; contention-observed=%p; release-owner=%p\n",
                g_mutex, g_ownerOwns, g_contenderReady, g_contentionObserved, g_releaseOwner);
    HANDLE threads[2]{};
    unsigned threadIds[2]{};
    if (success) {
        threads[0] = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, OwnerMain, nullptr, 0, &threadIds[0]));
        if (threads[0] == nullptr) {
            std::fprintf(stderr, "_beginthreadex(owner) failed: errno %d\n", errno);
            success = false;
        } else {
            std::printf("Owner TID=%u (0x%x), thread HANDLE=%p\n", threadIds[0], threadIds[0], threads[0]);
            success = NameThread(threads[0], L"mutex-owner") && AwaitPhase(g_ownerOwns, "Owner ownership");
        }
    }
    if (success) {
        std::printf("Phase 1: owner holds mutex. Starting contender.\n");
        threads[1] = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, ContenderMain, nullptr, 0, &threadIds[1]));
        if (threads[1] == nullptr) {
            std::fprintf(stderr, "_beginthreadex(contender) failed: errno %d\n", errno);
            success = false;
        } else {
            std::printf("Contender TID=%u (0x%x), thread HANDLE=%p\n", threadIds[1], threadIds[1], threads[1]);
            success = NameThread(threads[1], L"mutex-contender") &&
                      AwaitPhase(g_contenderReady, "Contender readiness") &&
                      AwaitPhase(g_contentionObserved, "Observed contention");
        }
    }
    if (success) success = Checkpoint(automatic);
    if (!success && g_abort != nullptr) Check(SetEvent(g_abort), "SetEvent(abort)");
    if (g_releaseOwner != nullptr) {
        std::printf("Phase 3: asking owner to release; main does not own or release the mutex itself.\n");
        if (!Check(SetEvent(g_releaseOwner), "SetEvent(release owner)")) success = false;
    }
    bool allJoined = true;
    for (unsigned index = 0; index < 2; ++index) {
        if (threads[index] == nullptr) continue;
        if (!Await(threads[index], "Worker join")) {
            allJoined = false;
            success = false;
            Check(SetEvent(g_abort), "SetEvent(join failure abort)");
            continue;
        }
        DWORD exitCode = 1;
        if (!Check(GetExitCodeThread(threads[index], &exitCode), "GetExitCodeThread")) success = false;
        WorkerState& state = index == 0 ? g_owner : g_contender;
        std::printf("%s: acquired=%d, abandoned=%d, exit=%lu, error=%lu\n", index == 0 ? "Owner" : "Contender",
                    state.acquired.load() ? 1 : 0, state.abandoned.load() ? 1 : 0, exitCode, state.error.load());
        if (state.abandoned.load())
            std::fprintf(stderr, "WAIT_ABANDONED acquired ownership but protected data may be inconsistent; released and failed.\n");
        if (exitCode != 0 || state.error.load() != ERROR_SUCCESS || !state.acquired.load()) success = false;
    }
    for (HANDLE thread : threads) if (thread != nullptr && !Check(CloseHandle(thread), "CloseHandle(thread)")) success = false;
    if (allJoined) {
        if (g_mutex != nullptr && !Check(CloseHandle(g_mutex), "CloseHandle(mutex)")) success = false;
        for (HANDLE* event : events) if (*event != nullptr && !Check(CloseHandle(*event), "CloseHandle(event)")) success = false;
    }
    std::printf("%s: owner then contender acquired serially; no arbitrary suspension.\n", success ? "PASS" : "FAIL");
    return success ? 0 : 1;
}