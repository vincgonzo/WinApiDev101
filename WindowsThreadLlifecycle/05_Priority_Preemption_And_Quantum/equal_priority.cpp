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
    std::atomic<DWORD> error{ERROR_SUCCESS};
};

HANDLE g_startEvent = nullptr;
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

bool CloseChecked(HANDLE handle) {
    return handle == nullptr || Check(CloseHandle(handle), "CloseHandle");
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
        const DWORD remaining = static_cast<DWORD>(deadline - now);
        const DWORD wait = WaitForSingleObject(input, remaining);
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
    const DWORD startWait = WaitForSingleObject(g_startEvent, RemainingMs());
    if (startWait != WAIT_OBJECT_0) {
        state.error.store(startWait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
        return 1;
    }
    const ULONGLONG started = GetTickCount64();
    unsigned long long value = 0x9e3779b97f4a7c15ULL + state.index;
    unsigned long long iterations = 0;
    unsigned exitCode = 0;
    while (GetTickCount64() - started < state.durationMs) {
        if (GetTickCount64() >= g_deadline) {
            state.error.store(ERROR_TIMEOUT);
            exitCode = 1;
            break;
        }
        const DWORD stopWait = WaitForSingleObject(g_stopEvent, 0);
        if (stopWait == WAIT_OBJECT_0) break;
        if (stopWait != WAIT_TIMEOUT) {
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
    return exitCode;
}

int main(int argumentCount, char** arguments) {
    const bool automatic = argumentCount == 2 && std::strcmp(arguments[1], "--auto") == 0;
    if (argumentCount > 1 && !automatic) {
        std::fprintf(stderr, "Usage: equal_priority.exe [--auto]\n");
        return 1;
    }
    g_deadline = GetTickCount64() + 18000;
    std::printf("PID=%lu (0x%lx), main TID=%lu (0x%lx)\n", GetCurrentProcessId(),
                GetCurrentProcessId(), GetCurrentThreadId(), GetCurrentThreadId());
    const WORD groups = GetActiveProcessorGroupCount();
    if (groups > 1) {
        std::printf("SKIP: %u active processor groups; this exercise is single-group only.\n",
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
        std::fprintf(stderr, "No eligible logical processor in the affinity masks.\n");
        return 1;
    }
    const DWORD_PTR chosenMask = eligibleMask & (~eligibleMask + static_cast<DWORD_PTR>(1));
    unsigned processor = 0;
    while ((chosenMask >> processor) != 1) ++processor;
    if (!Check(SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS), "SetPriorityClass") ||
        !Check(SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL), "SetThreadPriority(main)") ||
        !NameThread(GetCurrentThread(), L"equal-main")) return 1;
    if (GetPriorityClass(GetCurrentProcess()) != NORMAL_PRIORITY_CLASS) {
        std::fprintf(stderr, "Could not verify NORMAL_PRIORITY_CLASS: error %lu\n", GetLastError());
        return 1;
    }
    std::printf("Eligible mask=0x%llx; workers use group 0 CPU %u (mask=0x%llx).\n",
                static_cast<unsigned long long>(eligibleMask), processor,
                static_cast<unsigned long long>(chosenMask));
    std::printf("Main affinity is unchanged. Worker duration=%lu ms; equal configured priorities.\n",
                automatic ? 1000UL : 6000UL);
    g_startEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool success = Check(g_startEvent != nullptr, "CreateEventW(start)");
    if (success) {
        g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        success = Check(g_stopEvent != nullptr, "CreateEventW(stop)");
    }
    HANDLE threads[2]{};
    unsigned threadIds[2]{};
    for (unsigned index = 0; success && index < 2; ++index) {
        g_workers[index].index = index;
        g_workers[index].durationMs = automatic ? 1000 : 6000;
        const uintptr_t created = _beginthreadex(nullptr, 0, CpuWorker, &g_workers[index], 0,
                                                &threadIds[index]);
        threads[index] = reinterpret_cast<HANDLE>(created);
        if (threads[index] == nullptr) {
            std::fprintf(stderr, "_beginthreadex failed: errno %d\n", errno);
            success = false;
            break;
        }
        std::printf("Worker %u TID=%u (0x%x), HANDLE=%p\n", index, threadIds[index],
                    threadIds[index], threads[index]);
        success = NameThread(threads[index], index == 0 ? L"equal-cpu-a" : L"equal-cpu-b") &&
                  Check(SetThreadAffinityMask(threads[index], chosenMask) != 0, "SetThreadAffinityMask") &&
                  Check(SetThreadPriority(threads[index], THREAD_PRIORITY_NORMAL), "SetThreadPriority");
        if (success && GetThreadPriority(threads[index]) != THREAD_PRIORITY_NORMAL) {
            std::fprintf(stderr, "Worker %u priority verification failed: error %lu\n", index, GetLastError());
            success = false;
        }
    }
    if (success) success = Checkpoint(automatic, "Phase 1: workers gated; prepare trace. Release next.", 2000);
    if (success) {
        std::printf("Phase 2: common start event at uptime=%llu ms.\n",
                    static_cast<unsigned long long>(GetTickCount64()));
        success = Check(SetEvent(g_startEvent), "SetEvent(start)");
    }
    if (!success) {
        if (g_stopEvent != nullptr) Check(SetEvent(g_stopEvent), "SetEvent(cleanup stop)");
        if (g_startEvent != nullptr) Check(SetEvent(g_startEvent), "SetEvent(cleanup start)");
    }
    bool allJoined = true;
    for (unsigned index = 0; index < 2; ++index) {
        if (threads[index] == nullptr) continue;
        const DWORD wait = WaitForSingleObject(threads[index], RemainingMs());
        if (wait != WAIT_OBJECT_0) {
            std::fprintf(stderr, "Worker %u join failed: wait=0x%lx, error=%lu\n", index, wait,
                         wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
            allJoined = false;
            success = false;
            if (g_stopEvent != nullptr) Check(SetEvent(g_stopEvent), "SetEvent(join failure stop)");
            continue;
        }
        DWORD exitCode = 1;
        ULONG64 cycles = 0;
        if (!Check(GetExitCodeThread(threads[index], &exitCode), "GetExitCodeThread") ||
            !Check(QueryThreadCycleTime(threads[index], &cycles), "QueryThreadCycleTime")) success = false;
        std::printf("Worker %u: work=%llu iterations, elapsed=%llu ms, cycles=%llu, checksum=0x%llx, "
                    "exit=%lu, error=%lu\n", index, g_workers[index].iterations.load(),
                    g_workers[index].elapsedMs.load(), static_cast<unsigned long long>(cycles),
                    g_workers[index].checksum.load(), exitCode, g_workers[index].error.load());
        if (exitCode != 0 || g_workers[index].error.load() != ERROR_SUCCESS ||
            g_workers[index].iterations.load() == 0) success = false;
    }
    for (HANDLE thread : threads) if (!CloseChecked(thread)) success = false;
    if (allJoined) {
        if (!CloseChecked(g_startEvent)) success = false;
        if (!CloseChecked(g_stopEvent)) success = false;
    }
    std::printf("%s: counts and elapsed intervals are not scheduler quantum measurements.\n",
                success ? "PASS" : "FAIL");
    return success ? 0 : 1;
}