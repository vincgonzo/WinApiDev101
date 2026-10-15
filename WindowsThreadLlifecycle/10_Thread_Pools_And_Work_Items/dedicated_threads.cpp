#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>

constexpr DWORD TaskCount = 6;

struct TaskRecord {
    HANDLE readyEvent;
    HANDLE startEvent;
    DWORD gateTimeout;
    DWORD taskNumber;
    DWORD workerId;
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

void NameThread(HANDLE thread, const wchar_t* name) {
    using DescriptionFunction = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    HMODULE module = GetModuleHandleW(L"KernelBase.dll");
    Check(module != nullptr, "GetModuleHandleW(KernelBase)");
    const FARPROC address = GetProcAddress(module, "SetThreadDescription");
    Check(address != nullptr, "GetProcAddress(SetThreadDescription)");
    DescriptionFunction describeThread = nullptr;
    static_assert(sizeof(describeThread) == sizeof(address), "Function pointer sizes must match");
    std::memcpy(&describeThread, &address, sizeof(describeThread));
    const HRESULT result = describeThread(thread, name);
    if (FAILED(result)) Fail("SetThreadDescription", static_cast<DWORD>(result));
}

void WaitAll(const HANDLE* handles, const char* operation) {
    const DWORD result = WaitForMultipleObjects(TaskCount, handles, TRUE, 6000);
    if (result != WAIT_OBJECT_0) {
        Fail(operation, result == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
    }
}

__declspec(noinline) DWORD WINAPI DedicatedTask(void* parameter) {
    auto* record = static_cast<TaskRecord*>(parameter);
    record->workerId = GetCurrentThreadId();
    if (!SetEvent(record->readyEvent)) return GetLastError();
    const DWORD gateResult = WaitForSingleObject(record->startEvent, record->gateTimeout);
    if (gateResult != WAIT_OBJECT_0) {
        return gateResult == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
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
    return ERROR_SUCCESS;
}

int main(int argumentCount, char** arguments) {
    const bool automatic = argumentCount == 2 && std::strcmp(arguments[1], "--auto") == 0;
    if (argumentCount > 1 && !automatic) {
        std::fprintf(stderr, "Usage: dedicated_threads.exe [--auto]\n");
        return 1;
    }

    NameThread(GetCurrentThread(), L"dedicated-main");
    std::printf("Phase 1: PID=%lu (0x%lx), main TID=%lu (0x%lx)\n",
                GetCurrentProcessId(), GetCurrentProcessId(), GetCurrentThreadId(), GetCurrentThreadId());
    HANDLE startEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(startEvent != nullptr, "CreateEventW(start)");
    HANDLE workers[TaskCount]{};
    HANDLE readyEvents[TaskCount]{};
    DWORD workerIds[TaskCount]{};
    TaskRecord records[TaskCount]{};
    const wchar_t* names[TaskCount] = {
        L"dedicated-task-1", L"dedicated-task-2", L"dedicated-task-3",
        L"dedicated-task-4", L"dedicated-task-5", L"dedicated-task-6"
    };

    for (DWORD index = 0; index < TaskCount; ++index) {
        readyEvents[index] = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Check(readyEvents[index] != nullptr, "CreateEventW(ready)");
        records[index].readyEvent = readyEvents[index];
        records[index].startEvent = startEvent;
        records[index].gateTimeout = automatic ? 6000 : 120000;
        records[index].taskNumber = index + 1;
        workers[index] = CreateThread(nullptr, 0, DedicatedTask, &records[index], 0, &workerIds[index]);
        Check(workers[index] != nullptr, "CreateThread");
        NameThread(workers[index], names[index]);
    }

    WaitAll(readyEvents, "Wait for all six entries");
    bool valid = true;
    for (DWORD index = 0; index < TaskCount; ++index) {
        DWORD exitCode = 0;
        Check(GetExitCodeThread(workers[index], &exitCode), "GetExitCodeThread(before gate)");
        valid = valid && exitCode == STILL_ACTIVE;
        std::printf("Gate arrival: task=%lu, TID=%lu (0x%lx)\n",
                    index + 1, workerIds[index], workerIds[index]);
        for (DWORD earlier = 0; earlier < index; ++earlier) {
            valid = valid && workerIds[index] != workerIds[earlier];
        }
    }

    std::printf("Phase 2: six workers created before any join, all have reached the start gate.\n");
    if (!automatic) {
        std::printf("Inspect the six named workers, then press Enter to release them.\n");
        if (std::getchar() == EOF) std::printf("Console input ended; releasing workers.\n");
    }
    Check(SetEvent(startEvent), "SetEvent(start)");
    WaitAll(workers, "Join all six workers");

    std::printf("Phase 3: six task records after all joins.\n");
    for (DWORD index = 0; index < TaskCount; ++index) {
        DWORD exitCode = 0;
        Check(GetExitCodeThread(workers[index], &exitCode), "GetExitCodeThread(after join)");
        const TaskRecord& record = records[index];
        std::printf("Task %lu: TID=%lu (0x%lx), checksum=%llu, work=%llu ms, exit=%lu\n",
                    record.taskNumber, record.workerId, record.workerId,
                    static_cast<unsigned long long>(record.checksum),
                    static_cast<unsigned long long>(record.elapsedMilliseconds), exitCode);
        valid = valid && exitCode == ERROR_SUCCESS && record.completed == 1
                && record.workerId == workerIds[index] && record.checksum != 0;
        Check(CloseHandle(workers[index]), "CloseHandle(worker)");
        Check(CloseHandle(readyEvents[index]), "CloseHandle(ready)");
    }
    Check(CloseHandle(startEvent), "CloseHandle(start)");
    std::printf("Six distinct, concurrently alive dedicated workers: %s.\n", valid ? "PASS" : "FAIL");
    return valid ? 0 : 1;
}