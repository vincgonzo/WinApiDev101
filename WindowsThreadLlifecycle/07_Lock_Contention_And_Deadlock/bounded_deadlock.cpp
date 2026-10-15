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
    unsigned index = 0;
    std::atomic<DWORD> error{ERROR_SUCCESS};
    std::atomic<DWORD> secondResult{WAIT_FAILED};
    std::atomic<bool> firstOwned{false};
    std::atomic<bool> abandoned{false};
    std::atomic<unsigned long long> waitStartedMs{0};
    std::atomic<unsigned long long> waitEndedMs{0};
};

HANDLE g_mutexes[2]{};
HANDLE g_firstOwned[2]{};
HANDLE g_attempting[2]{};
HANDLE g_beginCrossWait = nullptr;
HANDLE g_abort = nullptr;
ULONGLONG g_deadline = 0;
DWORD g_crossWaitMs = 0;
WorkerState g_workers[2];

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

DWORD WaitPhase(HANDLE event) {
    const HANDLE handles[] = {g_abort, event};
    return WaitForMultipleObjects(2, handles, FALSE, RemainingMs());
}

DWORD WaitError(DWORD wait) {
    return wait == WAIT_FAILED ? GetLastError() :
           (wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED);
}

bool AwaitPhase(HANDLE event, const char* operation) {
    const DWORD wait = WaitPhase(event);
    if (wait == WAIT_OBJECT_0 + 1) return true;
    std::fprintf(stderr, "%s failed/aborted: wait=0x%lx, error=%lu\n", operation, wait, WaitError(wait));
    return false;
}

bool Checkpoint(bool automatic) {
    std::printf("Phase 3: cross-wait gate released; both workers announced cross-acquisition.\n");
    std::printf("Inspect now; exact simultaneous kernel waits need stacks/trace. Recovery is autonomous [%s; max 1000 ms].\n",
                automatic ? "auto: continuing" : "Enter to continue");
    if (automatic) return true;
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input == nullptr || input == INVALID_HANDLE_VALUE || !GetConsoleMode(input, &mode)) {
        std::printf("No console input; continuing.\n");
        return true;
    }
    const ULONGLONG localDeadline = GetTickCount64() + 1000;
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

unsigned FinishWorker(WorkerState& state, bool ownsFirst, bool ownsSecond, DWORD error) {
    const unsigned other = 1 - state.index;
    if (ownsSecond && !ReleaseMutex(g_mutexes[other])) error = GetLastError();
    if (ownsFirst && !ReleaseMutex(g_mutexes[state.index])) error = GetLastError();
    state.error.store(error);
    if (error != ERROR_SUCCESS) {
        if (!SetEvent(g_abort)) state.error.store(GetLastError());
        return 1;
    }
    return 0;
}

__declspec(noinline) unsigned __stdcall DeadlockWorker(void* argument) {
    auto& state = *static_cast<WorkerState*>(argument);
    const unsigned other = 1 - state.index;
    const HANDLE firstOrAbort[] = {g_abort, g_mutexes[state.index]};
    DWORD wait = WaitForMultipleObjects(2, firstOrAbort, FALSE, RemainingMs());
    if (wait == WAIT_ABANDONED_0 + 1) {
        state.abandoned.store(true);
        return FinishWorker(state, true, false, ERROR_ABANDONED_WAIT_0);
    }
    if (wait != WAIT_OBJECT_0 + 1) return FinishWorker(state, false, false, WaitError(wait));
    state.firstOwned.store(true);
    if (!SetEvent(g_firstOwned[state.index])) return FinishWorker(state, true, false, GetLastError());
    wait = WaitPhase(g_beginCrossWait);
    if (wait != WAIT_OBJECT_0 + 1) return FinishWorker(state, true, false, WaitError(wait));
    if (!SetEvent(g_attempting[state.index])) return FinishWorker(state, true, false, GetLastError());
    wait = WaitPhase(g_attempting[other]);
    if (wait != WAIT_OBJECT_0 + 1) return FinishWorker(state, true, false, WaitError(wait));
    const DWORD remaining = RemainingMs();
    const DWORD timeout = g_crossWaitMs < remaining ? g_crossWaitMs : remaining;
    const HANDLE secondOrAbort[] = {g_abort, g_mutexes[other]};
    state.waitStartedMs.store(GetTickCount64());
    wait = WaitForMultipleObjects(2, secondOrAbort, FALSE, timeout);
    const DWORD error = wait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    state.waitEndedMs.store(GetTickCount64());
    state.secondResult.store(wait);
    if (wait == WAIT_TIMEOUT) return FinishWorker(state, true, false, ERROR_SUCCESS);
    if (wait == WAIT_OBJECT_0 + 1) return FinishWorker(state, true, true, ERROR_SUCCESS);
    if (wait == WAIT_ABANDONED_0 + 1) {
        state.abandoned.store(true);
        return FinishWorker(state, true, true, ERROR_ABANDONED_WAIT_0);
    }
    return FinishWorker(state, true, false, wait == WAIT_FAILED ? error : ERROR_OPERATION_ABORTED);
}

int main(int argumentCount, char** arguments) {
    const bool automatic = argumentCount == 2 && std::strcmp(arguments[1], "--auto") == 0;
    if (argumentCount > 1 && !automatic) {
        std::fprintf(stderr, "Usage: bounded_deadlock.exe [--auto]\n");
        return 1;
    }
    g_deadline = GetTickCount64() + 18000;
    g_crossWaitMs = automatic ? 1000 : 15000;
    std::printf("PID=%lu (0x%lx), main TID=%lu (0x%lx); cross-wait timeout=%lu ms\n",
                GetCurrentProcessId(), GetCurrentProcessId(), GetCurrentThreadId(),
                GetCurrentThreadId(), g_crossWaitMs);
    if (!NameThread(GetCurrentThread(), L"deadlock-main")) return 1;
    HANDLE* events[] = {&g_firstOwned[0], &g_firstOwned[1], &g_attempting[0], &g_attempting[1],
                        &g_beginCrossWait, &g_abort};
    bool success = true;
    for (HANDLE* event : events) {
        *event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!Check(*event != nullptr, "CreateEventW")) {
            success = false;
            break;
        }
    }
    for (unsigned index = 0; success && index < 2; ++index) {
        g_mutexes[index] = CreateMutexW(nullptr, FALSE, nullptr);
        success = Check(g_mutexes[index] != nullptr, "CreateMutexW");
    }
    std::printf("Mutex A HANDLE=%p; mutex B HANDLE=%p; begin-cross-wait event=%p\n",
                g_mutexes[0], g_mutexes[1], g_beginCrossWait);
    HANDLE threads[2]{};
    unsigned threadIds[2]{};
    for (unsigned index = 0; success && index < 2; ++index) {
        g_workers[index].index = index;
        threads[index] = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, DeadlockWorker,
                                            &g_workers[index], 0, &threadIds[index]));
        if (threads[index] == nullptr) {
            std::fprintf(stderr, "_beginthreadex failed: errno %d\n", errno);
            success = false;
            break;
        }
        std::printf("Worker %u TID=%u (0x%x), thread HANDLE=%p; first=%p, second=%p\n", index,
                    threadIds[index], threadIds[index], threads[index], g_mutexes[index], g_mutexes[1 - index]);
        success = NameThread(threads[index], index == 0 ? L"deadlock-a-then-b" : L"deadlock-b-then-a");
    }
    if (success) success = AwaitPhase(g_firstOwned[0], "Worker A owns first mutex") &&
                           AwaitPhase(g_firstOwned[1], "Worker B owns first mutex");
    if (success) {
        std::printf("Phase 1: both first mutexes owned; cross-wait gate closed. No cross-wait cycle yet.\n");
        std::printf("Phase 2: releasing both cross-acquisitions now, uptime=%llu ms.\n",
                    static_cast<unsigned long long>(GetTickCount64()));
        success = Check(SetEvent(g_beginCrossWait), "SetEvent(begin cross wait)") &&
                  AwaitPhase(g_attempting[0], "Worker A cross-acquisition announced") &&
                  AwaitPhase(g_attempting[1], "Worker B cross-acquisition announced");
    }
    if (success) success = Checkpoint(automatic);
    if (!success && g_abort != nullptr) Check(SetEvent(g_abort), "SetEvent(abort)");
    std::printf("Waiting for finite timeouts/release recovery; do not terminate or suspend either worker.\n");
    bool allJoined = true;
    unsigned timeouts = 0;
    for (unsigned index = 0; index < 2; ++index) {
        if (threads[index] == nullptr) continue;
        const DWORD wait = WaitForSingleObject(threads[index], RemainingMs());
        if (wait != WAIT_OBJECT_0) {
            std::fprintf(stderr, "Worker %u join failed: wait=0x%lx, error=%lu\n", index, wait, WaitError(wait));
            allJoined = false;
            success = false;
            Check(SetEvent(g_abort), "SetEvent(join failure abort)");
            continue;
        }
        DWORD exitCode = 1;
        if (!Check(GetExitCodeThread(threads[index], &exitCode), "GetExitCodeThread")) success = false;
        const DWORD result = g_workers[index].secondResult.load();
        if (result == WAIT_TIMEOUT) ++timeouts;
        std::printf("Worker %u second wait=%s (0x%lx), start=%llu ms, end=%llu ms, exit=%lu, error=%lu\n",
                    index, result == WAIT_TIMEOUT ? "TIMEOUT: released first mutex" :
                    (result == WAIT_OBJECT_0 + 1 ? "ACQUIRED: released both mutexes" : "FAILURE"), result,
                    g_workers[index].waitStartedMs.load(), g_workers[index].waitEndedMs.load(),
                    exitCode, g_workers[index].error.load());
        if (g_workers[index].abandoned.load())
            std::fprintf(stderr, "WAIT_ABANDONED grants ownership but protected state is untrusted; released and failed.\n");
        if (exitCode != 0 || g_workers[index].error.load() != ERROR_SUCCESS ||
            !g_workers[index].firstOwned.load() ||
            (result != WAIT_TIMEOUT && result != WAIT_OBJECT_0 + 1)) success = false;
    }
    if (timeouts == 0) {
        std::fprintf(stderr, "Expected at least one second-mutex timeout.\n");
        success = false;
    }
    for (HANDLE thread : threads) if (thread != nullptr && !Check(CloseHandle(thread), "CloseHandle(thread)")) success = false;
    if (allJoined) {
        for (HANDLE mutex : g_mutexes) if (mutex != nullptr && !Check(CloseHandle(mutex), "CloseHandle(mutex)")) success = false;
        for (HANDLE* event : events) if (*event != nullptr && !Check(CloseHandle(*event), "CloseHandle(event)")) success = false;
    }
    std::printf("%s: %u timeout(s); one or both timeouts are valid recovery outcomes.\n",
                success ? "PASS" : "FAIL", timeouts);
    return success ? 0 : 1;
}