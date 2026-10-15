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

[[noreturn]] void Fail(const char* operation) {
    const DWORD error = GetLastError();
    std::fprintf(stderr, "%s failed: Win32 error %lu\n", operation, error);
    ExitProcess(1);
}

void Check(BOOL success, const char* operation) {
    if (!success) Fail(operation);
}

void NameThread(HANDLE thread, const wchar_t* name) {
    using DescriptionFunction = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    HMODULE module = GetModuleHandleW(L"KernelBase.dll");
    Check(module != nullptr, "GetModuleHandleW(KernelBase)");
    const FARPROC address = GetProcAddress(module, "SetThreadDescription");
    Check(address != nullptr, "GetProcAddress(SetThreadDescription)");
    DescriptionFunction describeThread = nullptr;
    static_assert(sizeof(describeThread) == sizeof(address), "Function pointer size mismatch");
    std::memcpy(&describeThread, &address, sizeof(describeThread));
    const HRESULT result = describeThread(thread, name);
    if (FAILED(result)) {
        std::fprintf(stderr, "SetThreadDescription failed: HRESULT 0x%08lx\n", static_cast<unsigned long>(result));
        ExitProcess(1);
    }
}

void Checkpoint(bool automatic, const char* message) {
    std::printf("%s%s\n", message, automatic ? " [automatic]" : " Press Enter.");
    if (!automatic) std::getchar();
}

__declspec(noinline) DWORD WINAPI WorkerMain(void*) {
    InterlockedExchange(&g_workerEntered, 1);
    if (!SetEvent(g_enteredEvent)) return 125;
    const DWORD waitResult = WaitForSingleObject(g_stopEvent, 120000);
    return waitResult == WAIT_OBJECT_0 ? 42 : 124;
}

int main(int argumentCount, char** arguments) {
    const bool automatic = argumentCount == 2 && std::strcmp(arguments[1], "--auto") == 0;
    if (argumentCount > 1 && !automatic) {
        std::fprintf(stderr, "Usage: suspended_start.exe [--auto]\n");
        return 1;
    }
    g_enteredEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(g_enteredEvent != nullptr && g_stopEvent != nullptr, "CreateEventW");
    DWORD workerId = 0;
    HANDLE worker = CreateThread(nullptr, 0, WorkerMain, nullptr, CREATE_SUSPENDED, &workerId);
    Check(worker != nullptr, "CreateThread");
    NameThread(worker, L"birth-suspended");
    NameThread(GetCurrentThread(), L"birth-coordinator");
    std::printf("PID=%lu main TID=%lu worker TID=%lu (hex 0x%lx), handle=%p\n",
                GetCurrentProcessId(), GetCurrentThreadId(), workerId, workerId, worker);
    const LONG enteredBeforeResume = InterlockedCompareExchange(&g_workerEntered, 0, 0);
    std::printf("Before resume: application entry flag=%ld (expected 0).\n", enteredBeforeResume);
    if (enteredBeforeResume != 0) return 1;
    Checkpoint(automatic, "Inspect the created, suspended thread. Resume it next.");
    const DWORD previousCount = ResumeThread(worker);
    Check(previousCount != static_cast<DWORD>(-1), "ResumeThread");
    std::printf("ResumeThread previous suspend count=%lu (expected 1).\n", previousCount);
    const DWORD enteredWait = WaitForSingleObject(g_enteredEvent, 10000);
    if (enteredWait != WAIT_OBJECT_0) {
        std::fprintf(stderr, "Worker did not signal entry: wait result=0x%lx\n", enteredWait);
        ExitProcess(1);
    }
    std::printf("Worker entered and is approaching or inside its event wait.\n");
    Checkpoint(automatic, "Inspect WorkerMain and its startup stack. Stop it next (120-second worker timeout).");
    Check(SetEvent(g_stopEvent), "SetEvent(stop)");
    const DWORD exitWait = WaitForSingleObject(worker, 10000);
    if (exitWait != WAIT_OBJECT_0) {
        std::fprintf(stderr, "Worker join failed: wait result=0x%lx\n", exitWait);
        ExitProcess(1);
    }
    DWORD exitCode = 0;
    Check(GetExitCodeThread(worker, &exitCode), "GetExitCodeThread");
    std::printf("Worker exit code=%lu (expected 42).\n", exitCode);
    CloseHandle(worker);
    CloseHandle(g_enteredEvent);
    CloseHandle(g_stopEvent);
    return previousCount == 1 && exitCode == 42 ? 0 : 1;
}