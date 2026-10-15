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

void WaitBoth(const HANDLE (&handles)[2], DWORD timeout, const char* operation) {
    const DWORD result = WaitForMultipleObjects(2, handles, TRUE, timeout);
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
        std::puts("Prepare tracing now; --auto skips the pre-run checkpoint.");
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

struct ArithmeticResult {
    std::uint64_t iterations = 0;
    std::uint64_t checksum = 0;
};

struct WorkerResult {
    ArithmeticResult arithmetic{};
    LARGE_INTEGER begin{};
    LARGE_INTEGER end{};
    GROUP_AFFINITY affinity{};
    PROCESSOR_NUMBER firstProcessor{};
    PROCESSOR_NUMBER lastProcessor{};
};

struct WorkerContext {
    HANDLE prepared = nullptr;
    HANDLE start = nullptr;
    HANDLE approaching = nullptr;
    DWORD gateTimeout = 0;
    DWORD slot = 0;
    ULONGLONG deadline = 0;
    WorkerResult result{};
};

constexpr unsigned batchSize = 4096;

__declspec(noinline) ArithmeticResult BurnCpuUntil(ULONGLONG deadline, std::uint64_t seed) {
    ArithmeticResult result{};
    std::uint64_t checksum = seed;
    while (GetTickCount64() < deadline) {
        for (unsigned iteration = 0; iteration < batchSize; ++iteration) {
            checksum ^= checksum >> 12;
            checksum ^= checksum << 25;
            checksum ^= checksum >> 27;
            checksum *= 0x2545F4914F6CDD1DULL;
        }
        result.iterations += batchSize;
    }
    result.checksum = checksum;
    return result;
}

__declspec(noinline) unsigned __stdcall AffinityWorker(void* argument) {
    auto& context = *static_cast<WorkerContext*>(argument);
    Check(SetEvent(context.prepared), "SetEvent(prepared)");
    WaitOne(context.start, context.gateTimeout, "WaitForSingleObject(start)");
    WorkerResult result{};
    Check(GetThreadGroupAffinity(GetCurrentThread(), &result.affinity), "GetThreadGroupAffinity");
    GetCurrentProcessorNumberEx(&result.firstProcessor);
    Check(QueryPerformanceCounter(&result.begin), "QueryPerformanceCounter(worker begin)");
    Check(SetEvent(context.approaching), "SetEvent(approaching)");
    const std::uint64_t seed = 0x9E3779B97F4A7C15ULL ^ (static_cast<std::uint64_t>(context.slot) + 1);
    result.arithmetic = BurnCpuUntil(context.deadline, seed);
    Check(QueryPerformanceCounter(&result.end), "QueryPerformanceCounter(worker end)");
    GetCurrentProcessorNumberEx(&result.lastProcessor);
    context.result = result;
    return 0;
}

int main(int argumentCount, char** arguments) {
    const bool automatic = argumentCount == 2 && std::strcmp(arguments[1], "--auto") == 0;
    if (argumentCount != 1 && !automatic) {
        std::fprintf(stderr, "Usage: single_cpu.exe [--auto]\n");
        return 1;
    }
    NameThread(GetCurrentThread(), L"single-cpu-coordinator");
    const DWORD processId = GetCurrentProcessId();
    const DWORD mainId = GetCurrentThreadId();
    std::printf("PID=%lu (0x%lx), main TID=%lu (0x%lx)\n", processId, processId, mainId, mainId);
    const WORD groupCount = GetActiveProcessorGroupCount();
    if (groupCount == 0) {
        const DWORD error = GetLastError();
        Fail("GetActiveProcessorGroupCount", error);
    }
    if (groupCount != 1) {
        std::printf("UNSUPPORTED (77): %u active processor groups; this demo requires exactly one.\n",
                    static_cast<unsigned>(groupCount));
        return 77;
    }
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    Check(GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask), "GetProcessAffinityMask");
    const DWORD_PTR eligibleMask = processMask & systemMask;
    std::printf("Group 0: process mask=0x%016llx, system mask=0x%016llx, eligible mask=0x%016llx\n",
                static_cast<unsigned long long>(processMask), static_cast<unsigned long long>(systemMask),
                static_cast<unsigned long long>(eligibleMask));
    DWORD selectedCpu = 0;
    DWORD_PTR selectedMask = 0;
    constexpr DWORD maskBits = static_cast<DWORD>(sizeof(DWORD_PTR) * 8);
    for (DWORD processor = 0; processor < maskBits; ++processor) {
        const DWORD_PTR candidate = static_cast<DWORD_PTR>(1) << processor;
        if ((eligibleMask & candidate) != 0) {
            selectedCpu = processor;
            selectedMask = candidate;
            break;
        }
    }
    if (selectedMask == 0) {
        std::puts("UNSUPPORTED (77): no eligible logical processor in the process affinity mask.");
        return 77;
    }
    Check(SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS), "SetPriorityClass(NORMAL)");
    GROUP_AFFINITY mainAffinityBefore{};
    Check(GetThreadGroupAffinity(GetCurrentThread(), &mainAffinityBefore), "GetThreadGroupAffinity(main before)");
    const DWORD durationMs = automatic ? 1000 : 6000;
    std::printf("Both workers will use logical processor %lu, mask=0x%016llx, for %lu ms.\n",
                selectedCpu, static_cast<unsigned long long>(selectedMask), durationMs);
    std::puts("NORMAL process/thread priorities; coordinator affinity is left unchanged.");

    HANDLE start = NewEvent();
    WorkerContext contexts[2]{};
    HANDLE threads[2]{};
    HANDLE prepared[2]{};
    HANDLE approaching[2]{};
    unsigned threadIds[2]{};
    CpuSample before[2]{};
    const wchar_t* descriptions[2] = {L"single-cpu-A", L"single-cpu-B"};
    for (DWORD slot = 0; slot < 2; ++slot) {
        auto& context = contexts[slot];
        context.prepared = NewEvent();
        context.start = start;
        context.approaching = NewEvent();
        context.gateTimeout = automatic ? 6000 : 45000;
        context.slot = slot;
        prepared[slot] = context.prepared;
        approaching[slot] = context.approaching;
        const uintptr_t rawHandle = _beginthreadex(nullptr, 0, AffinityWorker, &context, 0, &threadIds[slot]);
        if (rawHandle == 0) {
            const int error = errno;
            std::fprintf(stderr, "_beginthreadex failed: errno=%d\n", error);
            ExitProcess(1);
        }
        threads[slot] = reinterpret_cast<HANDLE>(rawHandle);
        NameThread(threads[slot], descriptions[slot]);
        Check(SetThreadPriority(threads[slot], THREAD_PRIORITY_NORMAL), "SetThreadPriority(NORMAL)");
        const DWORD_PTR previousMask = SetThreadAffinityMask(threads[slot], selectedMask);
        if (previousMask == 0) {
            const DWORD error = GetLastError();
            Fail("SetThreadAffinityMask", error);
        }
        std::printf("Worker %c TID=%u (0x%x): CPU=%lu, mask=0x%016llx, previous mask=0x%016llx\n",
                    static_cast<int>('A' + slot), threadIds[slot], threadIds[slot], selectedCpu,
                    static_cast<unsigned long long>(selectedMask), static_cast<unsigned long long>(previousMask));
    }
    WaitBoth(prepared, 3000, "WaitForMultipleObjects(prepared)");
    PreparationCheckpoint(automatic);
    LARGE_INTEGER frequency{}, released{}, joined{};
    Check(QueryPerformanceFrequency(&frequency), "QueryPerformanceFrequency");
    Verify(frequency.QuadPart > 0, "positive performance counter frequency");
    for (DWORD slot = 0; slot < 2; ++slot) {
        before[slot] = SampleThread(threads[slot]);
    }
    const ULONGLONG startTick = GetTickCount64();
    for (auto& context : contexts) {
        context.deadline = startTick + durationMs;
    }
    Check(QueryPerformanceCounter(&released), "QueryPerformanceCounter(release)");
    Check(SetEvent(start), "SetEvent(start)");
    WaitBoth(approaching, 2000, "WaitForMultipleObjects(approaching)");
    WaitBoth(threads, durationMs + 3000, "WaitForMultipleObjects(join)");
    Check(QueryPerformanceCounter(&joined), "QueryPerformanceCounter(join)");
    Verify(joined.QuadPart >= released.QuadPart, "wall-clock counter is monotonic");
    std::printf("Release uptime=%llu ms; gate-to-join wall time=%.3f ms\n", startTick,
                static_cast<double>(joined.QuadPart - released.QuadPart) * 1000.0 /
                    static_cast<double>(frequency.QuadPart));

    double totalCpuMs = 0.0;
    std::uint64_t totalIterations = 0;
    for (DWORD slot = 0; slot < 2; ++slot) {
        DWORD exitCode = 0;
        Check(GetExitCodeThread(threads[slot], &exitCode), "GetExitCodeThread");
        Verify(exitCode == 0, "worker exit code is zero");
        const CpuSample after = SampleThread(threads[slot]);
        const auto& result = contexts[slot].result;
        Verify(after.kernel >= before[slot].kernel && after.user >= before[slot].user &&
                   after.cycles >= before[slot].cycles, "thread accounting is monotonic");
        Verify(result.end.QuadPart >= result.begin.QuadPart, "worker wall-clock counter is monotonic");
        Verify(result.affinity.Group == 0 && result.affinity.Mask == selectedMask, "worker affinity matches placement");
        Verify(result.firstProcessor.Group == 0 && result.lastProcessor.Group == 0 &&
                   result.firstProcessor.Number == selectedCpu && result.lastProcessor.Number == selectedCpu,
               "processor snapshots match the pinned logical processor");
        Verify(result.arithmetic.iterations % batchSize == 0, "only complete arithmetic batches are published");
        const double kernelMs = static_cast<double>(after.kernel - before[slot].kernel) / 10000.0;
        const double userMs = static_cast<double>(after.user - before[slot].user) / 10000.0;
        const double runtimeMs = static_cast<double>(result.end.QuadPart - result.begin.QuadPart) * 1000.0 /
                                 static_cast<double>(frequency.QuadPart);
        totalCpuMs += kernelMs + userMs;
        totalIterations += result.arithmetic.iterations;
        std::printf("Worker %c TID=%u (0x%x): wall runtime=%.3f ms, CPU user=%.3f ms kernel=%.3f ms total=%.3f ms\n",
                    static_cast<int>('A' + slot), threadIds[slot], threadIds[slot], runtimeMs,
                    userMs, kernelMs, userMs + kernelMs);
        std::printf("  cycles=%llu, iterations=%llu, checksum=0x%016llx, first/last CPU=%u/%u\n",
                    after.cycles - before[slot].cycles,
                    static_cast<unsigned long long>(result.arithmetic.iterations),
                    static_cast<unsigned long long>(result.arithmetic.checksum),
                    static_cast<unsigned>(result.firstProcessor.Number),
                    static_cast<unsigned>(result.lastProcessor.Number));
        if (result.arithmetic.iterations == 0) {
            std::puts("  No batch ran before the shared deadline; scheduling/load prevented a useful workload sample.");
        }
    }
    GROUP_AFFINITY mainAffinityAfter{};
    Check(GetThreadGroupAffinity(GetCurrentThread(), &mainAffinityAfter), "GetThreadGroupAffinity(main after)");
    Verify(mainAffinityAfter.Group == mainAffinityBefore.Group && mainAffinityAfter.Mask == mainAffinityBefore.Mask,
           "coordinator affinity stayed unchanged");
    std::printf("Combined worker CPU=%.3f ms, iterations=%llu\n", totalCpuMs,
                static_cast<unsigned long long>(totalIterations));
    std::puts("Compare with two_cpus.exe. Counters do not prove Ready states, fairness, or speedup; use WPA CPU Usage (Precise).");
    for (DWORD slot = 0; slot < 2; ++slot) {
        Check(CloseHandle(threads[slot]), "CloseHandle(thread)");
        Check(CloseHandle(prepared[slot]), "CloseHandle(prepared)");
        Check(CloseHandle(approaching[slot]), "CloseHandle(approaching)");
    }
    Check(CloseHandle(start), "CloseHandle(start)");
    std::puts("PASS: both workers joined with the requested single-CPU placement and published results.");
    return 0;
}