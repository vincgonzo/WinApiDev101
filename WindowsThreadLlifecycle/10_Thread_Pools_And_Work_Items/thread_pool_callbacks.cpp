#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>

constexpr DWORD TaskCount = 6;
using DescriptionFunction = HRESULT(WINAPI*)(HANDLE, PCWSTR);
DescriptionFunction g_describeThread = nullptr;

struct PoolCounters {
    LONG active;
    LONG peak;
};

struct WorkRecord {
    PoolCounters* counters;
    DWORD taskNumber;
    DWORD callbackThreadId;
    HRESULT nameResult;
    DWORD completed;
    ULONGLONG checksum;
    ULONGLONG elapsedMilliseconds;
};

[[noreturn]] void Fail(const char* operation, DWORD error) {
    std::fprintf(stderr, "%s failed: Win32 error %lu (0x%lx)\n", operation, error, error);
    ExitProcess(1);
}

void Check(BOOL success, const char* operation) {
    if (!success) Fail(operation, GetLastError());
}

void InitializeThreadNaming() {
    HMODULE module = GetModuleHandleW(L"KernelBase.dll");
    Check(module != nullptr, "GetModuleHandleW(KernelBase)");
    const FARPROC address = GetProcAddress(module, "SetThreadDescription");
    Check(address != nullptr, "GetProcAddress(SetThreadDescription)");
    static_assert(sizeof(g_describeThread) == sizeof(address), "Function pointer sizes must match");
    std::memcpy(&g_describeThread, &address, sizeof(g_describeThread));
}

void Checkpoint(bool automatic, const char* message) {
    std::printf("%s%s\n", message, automatic ? " [automatic]" : " Press Enter.");
    if (!automatic && std::getchar() == EOF) std::printf("Console input ended; continuing.\n");
}

__declspec(noinline) void CALLBACK PoolWorkCallback(PTP_CALLBACK_INSTANCE, void* parameter, PTP_WORK) {
    auto* record = static_cast<WorkRecord*>(parameter);
    record->callbackThreadId = GetCurrentThreadId();
    record->nameResult = g_describeThread(GetCurrentThread(), L"private-pool-worker");
    const LONG active = InterlockedIncrement(&record->counters->active);
    LONG observedPeak = InterlockedCompareExchange(&record->counters->peak, 0, 0);
    while (active > observedPeak) {
        const LONG previousPeak = InterlockedCompareExchange(&record->counters->peak, active, observedPeak);
        if (previousPeak == observedPeak) break;
        observedPeak = previousPeak;
    }

    const ULONGLONG started = GetTickCount64();
    ULONGLONG checksum = static_cast<ULONGLONG>(record->taskNumber) + 1;
    for (DWORD step = 0; step < 500000; ++step) {
        checksum ^= checksum << 13;
        checksum ^= checksum >> 7;
        checksum ^= checksum << 17;
    }
    Sleep(150);
    record->checksum = checksum;
    record->elapsedMilliseconds = GetTickCount64() - started;
    record->completed = 1;
    InterlockedDecrement(&record->counters->active);
}

int main(int argumentCount, char** arguments) {
    const bool automatic = argumentCount == 2 && std::strcmp(arguments[1], "--auto") == 0;
    if (argumentCount > 1 && !automatic) {
        std::fprintf(stderr, "Usage: thread_pool_callbacks.exe [--auto]\n");
        return 1;
    }
    InitializeThreadNaming();
    const HRESULT nameResult = g_describeThread(GetCurrentThread(), L"pool-main");
    if (FAILED(nameResult)) Fail("SetThreadDescription(main)", static_cast<DWORD>(nameResult));
    std::printf("Phase 1: PID=%lu (0x%lx), main TID=%lu (0x%lx)\n",
                GetCurrentProcessId(), GetCurrentProcessId(), GetCurrentThreadId(), GetCurrentThreadId());

    PTP_POOL pool = CreateThreadpool(nullptr);
    Check(pool != nullptr, "CreateThreadpool");
    SetThreadpoolThreadMaximum(pool, 2);
    Check(SetThreadpoolThreadMinimum(pool, 1), "SetThreadpoolThreadMinimum");
    TP_CALLBACK_ENVIRON environment;
    InitializeThreadpoolEnvironment(&environment);
    SetThreadpoolCallbackPool(&environment, pool);
    PoolCounters counters{};
    WorkRecord records[TaskCount]{};
    PTP_WORK workItems[TaskCount]{};

    for (DWORD index = 0; index < TaskCount; ++index) {
        records[index].taskNumber = index + 1;
        records[index].counters = &counters;
        workItems[index] = CreateThreadpoolWork(PoolWorkCallback, &records[index], &environment);
        Check(workItems[index] != nullptr, "CreateThreadpoolWork");
    }
    std::printf("Private pool configured: minimum=1, maximum=2; six independent work items.\n");
    Checkpoint(automatic, "Set a breakpoint on PoolWorkCallback before submission if desired.");

    std::printf("Phase 2: submit all six items; wait for each with cancellation=FALSE.\n");
    for (DWORD index = 0; index < TaskCount; ++index) SubmitThreadpoolWork(workItems[index]);
    for (DWORD index = 0; index < TaskCount; ++index) {
        WaitForThreadpoolWorkCallbacks(workItems[index], FALSE);
    }

    bool valid = true;
    DWORD distinctThreadIds = 0;
    for (DWORD index = 0; index < TaskCount; ++index) {
        const WorkRecord& record = records[index];
        std::printf("Task %lu: callback TID=%lu (0x%lx), checksum=%llu, work=%llu ms, name HRESULT=0x%08lx\n",
                    record.taskNumber, record.callbackThreadId, record.callbackThreadId,
                    static_cast<unsigned long long>(record.checksum),
                    static_cast<unsigned long long>(record.elapsedMilliseconds),
                    static_cast<unsigned long>(record.nameResult));
        valid = valid && record.completed == 1 && record.callbackThreadId != 0
                && record.checksum != 0 && SUCCEEDED(record.nameResult);
        bool seenEarlier = false;
        for (DWORD earlier = 0; earlier < index; ++earlier) {
            if (records[earlier].callbackThreadId == record.callbackThreadId) seenEarlier = true;
        }
        if (!seenEarlier) ++distinctThreadIds;
    }
    const LONG peak = InterlockedCompareExchange(&counters.peak, 0, 0);
    valid = valid && peak >= 1 && peak <= 2 && InterlockedCompareExchange(&counters.active, 0, 0) == 0;
    std::printf("Distinct callback TIDs=%lu; assignments reusing an earlier TID=%lu; peak active callbacks=%ld.\n",
                distinctThreadIds, TaskCount - distinctThreadIds, peak);
    std::printf("Reuse evidence in this run: %s. Using both allowed workers is not required.\n",
                distinctThreadIds < TaskCount ? "observed" : "not observed");

    std::printf("Phase 3: callbacks finished; pool and work objects still exist.\n");
    Checkpoint(automatic, "Inspect idle pool threads; only a callback breakpoint shows the callback stack itself.");
    for (DWORD index = 0; index < TaskCount; ++index) CloseThreadpoolWork(workItems[index]);
    DestroyThreadpoolEnvironment(&environment);
    CloseThreadpool(pool);
    std::printf("All work objects, callback environment, and private pool closed: %s.\n", valid ? "PASS" : "FAIL");
    return valid ? 0 : 1;
}