#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <process.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

static_assert(sizeof(void*) == 8, "Build this teaching example for x64.");

[[noreturn]] void Fail(const char* operation, DWORD error) {
    std::fprintf(stderr, "%s failed: code=%lu (0x%08lx)\n", operation, error, error);
    ExitProcess(1);
}

void Check(BOOL success, const char* operation) {
    if (!success) {
        const DWORD error = GetLastError();
        Fail(operation, error);
    }
}

void Verify(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "Check failed: %s\n", description);
        ExitProcess(1);
    }
}

HANDLE NewEvent() {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(event != nullptr, "CreateEventW");
    return event;
}

void WaitOne(HANDLE handle, DWORD timeout, const char* operation) {
    const DWORD result = WaitForSingleObject(handle, timeout);
    if (result != WAIT_OBJECT_0) {
        const DWORD error = result == WAIT_FAILED ? GetLastError() : result;
        Fail(operation, error);
    }
}

void NameThread(HANDLE thread, const wchar_t* description) {
    HMODULE module = GetModuleHandleW(L"KernelBase.dll");
    Check(module != nullptr, "GetModuleHandleW(KernelBase.dll)");
    const FARPROC address = GetProcAddress(module, "SetThreadDescription");
    Check(address != nullptr, "GetProcAddress(SetThreadDescription)");
    using Setter = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    Setter setter = nullptr;
    static_assert(sizeof(setter) == sizeof(address), "Function pointer size mismatch.");
    std::memcpy(&setter, &address, sizeof(setter));
    const HRESULT result = setter(thread, description);
    if (FAILED(result)) {
        Fail("SetThreadDescription", static_cast<DWORD>(result));
    }
}

void PreparationCheckpoint(bool automatic) {
    if (automatic) {
        std::puts("Prepare Process Explorer/WPR now; --auto skips the pre-run checkpoint.");
        return;
    }
    std::puts("Prepare Process Explorer/WPR now. Press Enter to run, or wait 30 seconds.");
    std::fflush(stdout);
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input == nullptr || input == INVALID_HANDLE_VALUE || !GetConsoleMode(input, &mode)) {
        std::puts("No interactive console input; starting without a checkpoint.");
        return;
    }
    const ULONGLONG deadline = GetTickCount64() + 30000;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            return;
        }
        const DWORD result = WaitForSingleObject(input, static_cast<DWORD>(deadline - now));
        if (result == WAIT_TIMEOUT) {
            return;
        }
        if (result != WAIT_OBJECT_0) {
            const DWORD error = result == WAIT_FAILED ? GetLastError() : result;
            Fail("WaitForSingleObject(console)", error);
        }
        INPUT_RECORD record{};
        DWORD count = 0;
        Check(ReadConsoleInputW(input, &record, 1, &count), "ReadConsoleInputW");
        if (count == 1 && record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown &&
            record.Event.KeyEvent.wVirtualKeyCode == VK_RETURN) {
            return;
        }
    }
}

ULONGLONG FileTimeTicks(const FILETIME& value) {
    ULARGE_INTEGER ticks{};
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    return ticks.QuadPart;
}

struct CpuSample {
    ULONGLONG kernel = 0;
    ULONGLONG user = 0;
    ULONG64 cycles = 0;
};

CpuSample SampleThread(HANDLE thread) {
    FILETIME created{}, exited{}, kernel{}, user{};
    Check(GetThreadTimes(thread, &created, &exited, &kernel, &user), "GetThreadTimes");
    CpuSample sample{};
    sample.kernel = FileTimeTicks(kernel);
    sample.user = FileTimeTicks(user);
    Check(QueryThreadCycleTime(thread, &sample.cycles), "QueryThreadCycleTime");
    return sample;
}

struct WorkerContext {
    HANDLE prepared = nullptr;
    HANDLE start = nullptr;
    HANDLE approaching = nullptr;
    HANDLE completion = nullptr;
    DWORD gateTimeout = 0;
    DWORD workTimeout = 0;
    DWORD observedWait = WAIT_FAILED;
    unsigned long long polls = 0;
};

__declspec(noinline) unsigned long long PollUntilSignaled(HANDLE completion, DWORD timeout) {
    const ULONGLONG deadline = GetTickCount64() + timeout;
    unsigned long long polls = 0;
    for (;;) {
        const DWORD result = WaitForSingleObject(completion, 0);
        if (result == WAIT_FAILED) {
            const DWORD error = GetLastError();
            Fail("WaitForSingleObject(zero-timeout poll)", error);
        }
        ++polls;
        if (result == WAIT_OBJECT_0) {
            return polls;
        }
        if (result != WAIT_TIMEOUT) {
            Fail("Unexpected event polling result", result);
        }
        if (GetTickCount64() >= deadline) {
            Fail("PollUntilSignaled safety deadline", WAIT_TIMEOUT);
        }
    }
}

__declspec(noinline) unsigned __stdcall PollingEventWorker(void* argument) {
    auto& context = *static_cast<WorkerContext*>(argument);
    Check(SetEvent(context.prepared), "SetEvent(prepared)");
    WaitOne(context.start, context.gateTimeout, "WaitForSingleObject(start)");
    Check(SetEvent(context.approaching), "SetEvent(approaching)");
    context.polls = PollUntilSignaled(context.completion, context.workTimeout);
    context.observedWait = WAIT_OBJECT_0;
    return 0;
}

int main(int argumentCount, char** arguments) {
    const bool automatic = argumentCount == 2 && std::strcmp(arguments[1], "--auto") == 0;
    if (argumentCount != 1 && !automatic) {
        std::fprintf(stderr, "Usage: polling_wait.exe [--auto]\n");
        return 1;
    }
    NameThread(GetCurrentThread(), L"polling-coordinator");
    const DWORD processId = GetCurrentProcessId();
    const DWORD mainId = GetCurrentThreadId();
    const DWORD observationMs = automatic ? 1000 : 5000;
    std::printf("PID=%lu (0x%lx), main TID=%lu (0x%lx)\n", processId, processId, mainId, mainId);

    WorkerContext context{};
    context.prepared = NewEvent();
    context.start = NewEvent();
    context.approaching = NewEvent();
    context.completion = NewEvent();
    context.gateTimeout = automatic ? 6000 : 40000;
    context.workTimeout = automatic ? 6000 : 12000;
    unsigned workerId = 0;
    const uintptr_t rawHandle = _beginthreadex(nullptr, 0, PollingEventWorker, &context, 0, &workerId);
    if (rawHandle == 0) {
        const int error = errno;
        std::fprintf(stderr, "_beginthreadex failed: errno=%d\n", error);
        ExitProcess(1);
    }
    HANDLE worker = reinterpret_cast<HANDLE>(rawHandle);
    NameThread(worker, L"polling-event-check");
    std::printf("Worker TID=%u (0x%x), name=polling-event-check, handle=%p\n", workerId, workerId, worker);
    WaitOne(context.prepared, 3000, "WaitForSingleObject(prepared)");
    std::printf("Run: %lu ms with an initially unsignaled manual-reset event.\n", observationMs);
    PreparationCheckpoint(automatic);

    LARGE_INTEGER frequency{}, startTime{}, endTime{};
    Check(QueryPerformanceFrequency(&frequency), "QueryPerformanceFrequency");
    Verify(frequency.QuadPart > 0, "positive performance counter frequency");
    const CpuSample before = SampleThread(worker);
    const ULONGLONG startTick = GetTickCount64();
    Check(QueryPerformanceCounter(&startTime), "QueryPerformanceCounter(start)");
    Check(SetEvent(context.start), "SetEvent(start)");
    WaitOne(context.approaching, 2000, "WaitForSingleObject(approaching)");
    std::puts("Worker acknowledged the polling approach. A zero timeout never blocks for the event.");
    const DWORD observationResult = WaitForSingleObject(worker, observationMs);
    if (observationResult != WAIT_TIMEOUT) {
        const DWORD error = observationResult == WAIT_FAILED ? GetLastError() : observationResult;
        Fail("Worker must remain alive until completion is signaled", error);
    }
    Check(SetEvent(context.completion), "SetEvent(completion)");
    WaitOne(worker, 3000, "WaitForSingleObject(join)");
    Check(QueryPerformanceCounter(&endTime), "QueryPerformanceCounter(end)");
    const CpuSample after = SampleThread(worker);
    DWORD exitCode = 0;
    Check(GetExitCodeThread(worker, &exitCode), "GetExitCodeThread");
    Verify(exitCode == 0 && context.observedWait == WAIT_OBJECT_0 && context.polls != 0,
           "worker polled, observed event satisfaction, and exited");
    Verify(after.kernel >= before.kernel && after.user >= before.user && after.cycles >= before.cycles,
           "thread accounting is monotonic");
    Verify(endTime.QuadPart >= startTime.QuadPart, "wall-clock counter is monotonic");
    const double wallMs = static_cast<double>(endTime.QuadPart - startTime.QuadPart) * 1000.0 /
                          static_cast<double>(frequency.QuadPart);
    const double kernelMs = static_cast<double>(after.kernel - before.kernel) / 10000.0;
    const double userMs = static_cast<double>(after.user - before.user) / 10000.0;
    std::printf("Start uptime=%llu ms; measured gate-to-join wall time=%.3f ms\n", startTick, wallMs);
    std::printf("Worker CPU delta: user=%.3f ms, kernel=%.3f ms, total=%.3f ms\n",
                userMs, kernelMs, userMs + kernelMs);
    std::printf("Worker QueryThreadCycleTime delta=%llu cycles; zero-timeout polls=%llu\n",
                after.cycles - before.cycles, context.polls);
    std::puts("Compare these worker deltas with blocking_wait.exe under similar conditions; no CPU threshold is asserted.");
    std::puts("Event satisfaction does not schedule the poller instantly; the join proves eventual completion.");

    Check(CloseHandle(worker), "CloseHandle(worker)");
    Check(CloseHandle(context.prepared), "CloseHandle(prepared)");
    Check(CloseHandle(context.start), "CloseHandle(start)");
    Check(CloseHandle(context.approaching), "CloseHandle(approaching)");
    Check(CloseHandle(context.completion), "CloseHandle(completion)");
    std::puts("PASS: bounded polling consumed the event signal and the worker joined.");
    return 0;
}