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
    DWORD durationMs = 0;
    std::atomic<unsigned long long> iterations{0};
    std::atomic<unsigned long long> elapsedMs{0};
    std::atomic<unsigned long long> checksum{0};
    std::atomic<long long> beginQpc{0};
    std::atomic<long long> endQpc{0};
    std::atomic<DWORD> error{ERROR_SUCCESS};
};

HANDLE g_startEvents[2]{};
HANDLE g_readyEvents[2]{};
HANDLE g_stopEvent = nullptr;
ULONGLONG g_deadline = 0;
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

bool Await(HANDLE handle, const char* operation) {
    const DWORD wait = WaitForSingleObject(handle, RemainingMs());
    if (wait == WAIT_OBJECT_0) return true;
    std::fprintf(stderr, "%s: wait=0x%lx, error=%lu\n", operation, wait,
                 wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
    return false;
}

bool Stamp(const char* phase) {
    LARGE_INTEGER counter{};
    if (!Check(QueryPerformanceCounter(&counter), "QueryPerformanceCounter")) return false;
    std::printf("%s: PID=%lu main TID=%lu (0x%lx), QPC=%lld, uptime=%llu ms\n", phase,
                GetCurrentProcessId(), GetCurrentThreadId(), GetCurrentThreadId(),
                static_cast<long long>(counter.QuadPart),
                static_cast<unsigned long long>(GetTickCount64()));
    return true;
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

__declspec(noinline) unsigned __stdcall CpuWorker(void* argument) {
    auto& state = *static_cast<WorkerState*>(argument);
    if (state.index == 1 && !SetEvent(g_readyEvents[1])) {
        state.error.store(GetLastError());
        return 1;
    }
    const DWORD wait = WaitForSingleObject(g_startEvents[state.index], RemainingMs());
    if (wait != WAIT_OBJECT_0) {
        state.error.store(wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
        return 1;
    }
    LARGE_INTEGER counter{};
    if (!QueryPerformanceCounter(&counter)) {
        state.error.store(GetLastError());
        return 1;
    }
    state.beginQpc.store(counter.QuadPart);
    if (state.index == 0 && !SetEvent(g_readyEvents[0])) {
        state.error.store(GetLastError());
        return 1;
    }
    const ULONGLONG started = GetTickCount64();
    unsigned long long value = 0xd1b54a32d192ed03ULL + state.index;
    unsigned long long iterations = 0;
    unsigned exitCode = 0;
    while (GetTickCount64() - started < state.durationMs) {
        if (GetTickCount64() >= g_deadline) {
            state.error.store(ERROR_TIMEOUT);
            exitCode = 1;
            break;
        }
        const DWORD stop = WaitForSingleObject(g_stopEvent, 0);
        if (stop == WAIT_OBJECT_0) break;
        if (stop != WAIT_TIMEOUT) {
            state.error.store(GetLastError());
            exitCode = 1;
            break;
        }
        for (unsigned step = 0; step < 4096; ++step) {
            value ^= value << 7;
            value ^= value >> 9;
            value = value * 2862933555777941757ULL + 3037000493ULL;
        }
        iterations += 4096;
    }
    state.iterations.store(iterations);
    state.elapsedMs.store(GetTickCount64() - started);
    state.checksum.store(value);
    if (!QueryPerformanceCounter(&counter)) {
        state.error.store(GetLastError());
        return 1;
    }
    state.endQpc.store(counter.QuadPart);
    return exitCode;
}

int main(int argumentCount, char** arguments) {
    const bool automatic = argumentCount == 2 && std::strcmp(arguments[1], "--auto") == 0;
    if (argumentCount > 1 && !automatic) {
        std::fprintf(stderr, "Usage: higher_priority_preemption.exe [--auto]\n");
        return 1;
    }
    g_deadline = GetTickCount64() + 18000;
    std::printf("PID=%lu (0x%lx), main TID=%lu (0x%lx)\n", GetCurrentProcessId(),
                GetCurrentProcessId(), GetCurrentThreadId(), GetCurrentThreadId());
    const WORD groups = GetActiveProcessorGroupCount();
    if (groups > 1) {
        std::printf("SKIP: %u active processor groups; single-group exercise.\n",
                    static_cast<unsigned>(groups));
        return 77;
    }
    if (groups == 0) {
        std::fprintf(stderr, "GetActiveProcessorGroupCount returned zero.\n");
        return 1;
    }
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    if (!Check(GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask),
               "GetProcessAffinityMask")) return 1;
    const DWORD_PTR eligibleMask = processMask & systemMask;
    if (eligibleMask == 0) {
        std::fprintf(stderr, "No eligible logical processor.\n");
        return 1;
    }
    const DWORD_PTR chosenMask = eligibleMask & (~eligibleMask + static_cast<DWORD_PTR>(1));
    unsigned processor = 0;
    while ((chosenMask >> processor) != 1) ++processor;
    LARGE_INTEGER frequency{};
    if (!Check(SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS), "SetPriorityClass") ||
        !Check(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL), "SetThreadPriority(main)") ||
        !NameThread(GetCurrentThread(), L"preemption-main") ||
        !Check(QueryPerformanceFrequency(&frequency), "QueryPerformanceFrequency")) return 1;
    const DWORD priorityClass = GetPriorityClass(GetCurrentProcess());
    if (priorityClass != NORMAL_PRIORITY_CLASS) {
        std::fprintf(stderr, "NORMAL_PRIORITY_CLASS verification failed: class=0x%lx\n", priorityClass);
        return 1;
    }
    std::printf("QPC frequency=%lld ticks/sec; eligible mask=0x%llx; worker CPU=%u, mask=0x%llx.\n",
                static_cast<long long>(frequency.QuadPart), static_cast<unsigned long long>(eligibleMask),
                processor, static_cast<unsigned long long>(chosenMask));
    std::printf("Main affinity unchanged; process class NORMAL. Priorities below are configured, not live dynamic values.\n");
    HANDLE* events[] = {&g_startEvents[0], &g_startEvents[1], &g_readyEvents[0],
                        &g_readyEvents[1], &g_stopEvent};
    bool success = true;
    for (HANDLE* event : events) {
        *event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!Check(*event != nullptr, "CreateEventW")) {
            success = false;
            break;
        }
    }
    HANDLE threads[2]{};
    unsigned threadIds[2]{};
    for (unsigned index = 0; success && index < 2; ++index) {
        g_workers[index].index = index;
        g_workers[index].durationMs = index == 0 ? (automatic ? 1000 : 6000) : 500;
        threads[index] = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, CpuWorker,
                                            &g_workers[index], 0, &threadIds[index]));
        if (threads[index] == nullptr) {
            std::fprintf(stderr, "_beginthreadex failed: errno %d\n", errno);
            success = false;
            break;
        }
        const int desired = index == 0 ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_ABOVE_NORMAL;
        std::printf("Worker %u TID=%u (0x%x), HANDLE=%p, requested relative priority=%d\n",
                    index, threadIds[index], threadIds[index], threads[index], desired);
        success = NameThread(threads[index], index == 0 ? L"preemption-normal" : L"preemption-above") &&
                  Check(SetThreadAffinityMask(threads[index], chosenMask) != 0, "SetThreadAffinityMask") &&
                  Check(SetThreadPriority(threads[index], desired), "SetThreadPriority");
        if (success) {
            const int actual = GetThreadPriority(threads[index]);
            if (actual != desired) {
                std::fprintf(stderr, "GetThreadPriority returned %d, expected %d\n", actual, desired);
                success = false;
            }
        }
    }
    if (success) success = Await(g_readyEvents[1], "High worker wake-wait readiness");
    if (success) success = Checkpoint(automatic, "Phase 1: workers gated; prepare scheduler trace.", 2000);
    if (success) success = Stamp("Phase 2: release normal CPU worker") &&
                           Check(SetEvent(g_startEvents[0]), "SetEvent(normal start)") &&
                           Await(g_readyEvents[0], "Normal worker entry");
    if (success && automatic) {
        const DWORD wait = WaitForSingleObject(g_stopEvent, 250);
        if (wait != WAIT_TIMEOUT) {
            std::fprintf(stderr, "Initial observation interval failed: wait=0x%lx, error=%lu\n", wait,
                         wait == WAIT_FAILED ? GetLastError() : ERROR_OPERATION_ABORTED);
            success = false;
        }
    }
    if (success) success = Checkpoint(automatic, "Normal worker busy; high worker gated. Wake high next.", 1000);
    if (success) success = Stamp("Phase 3: SetEvent for above-normal worker follows") &&
                           Check(SetEvent(g_startEvents[1]), "SetEvent(high wake)");
    if (!success) {
        if (g_stopEvent != nullptr) Check(SetEvent(g_stopEvent), "SetEvent(cleanup stop)");
        for (HANDLE event : g_startEvents) if (event != nullptr) Check(SetEvent(event), "SetEvent(cleanup start)");
    }
    bool allJoined = true;
    for (unsigned index = 0; index < 2; ++index) {
        if (threads[index] == nullptr) continue;
        if (!Await(threads[index], "Worker join")) {
            allJoined = false;
            success = false;
            Check(SetEvent(g_stopEvent), "SetEvent(join failure stop)");
            continue;
        }
        DWORD exitCode = 1;
        ULONG64 cycles = 0;
        if (!Check(GetExitCodeThread(threads[index], &exitCode), "GetExitCodeThread") ||
            !Check(QueryThreadCycleTime(threads[index], &cycles), "QueryThreadCycleTime")) success = false;
        std::printf("Worker %u TID=%u (0x%x): beginQPC=%lld endQPC=%lld elapsed=%llu ms, "
                    "work=%llu, cycles=%llu, checksum=0x%llx, exit=%lu, error=%lu\n", index,
                    threadIds[index], threadIds[index], g_workers[index].beginQpc.load(),
                    g_workers[index].endQpc.load(), g_workers[index].elapsedMs.load(),
                    g_workers[index].iterations.load(), static_cast<unsigned long long>(cycles),
                    g_workers[index].checksum.load(), exitCode, g_workers[index].error.load());
        if (exitCode != 0 || g_workers[index].error.load() != ERROR_SUCCESS ||
            g_workers[index].iterations.load() == 0) success = false;
    }
    for (HANDLE thread : threads) if (thread != nullptr && !Check(CloseHandle(thread), "CloseHandle(thread)")) success = false;
    if (allJoined) {
        for (HANDLE* event : events) if (*event != nullptr && !Check(CloseHandle(*event), "CloseHandle(event)")) success = false;
    }
    std::printf("%s: use the trace for dispatch preference; no timing/fairness assertion.\n", success ? "PASS" : "FAIL");
    return success ? 0 : 1;
}