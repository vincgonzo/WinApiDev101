#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>

struct WorkerContext {
    HANDLE deepestEntered;
    HANDLE releaseWorker;
    DWORD waitMilliseconds;
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

void WaitChecked(HANDLE handle, DWORD timeout, const char* operation) {
    const DWORD result = WaitForSingleObject(handle, timeout);
    if (result != WAIT_OBJECT_0) {
        Fail(operation, result == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
    }
}

void Checkpoint(bool automatic, const char* message) {
    std::printf("%s%s\n", message, automatic ? " [automatic]" : " Press Enter.");
    if (!automatic && std::getchar() == EOF) {
        std::printf("Console input ended; continuing cooperatively.\n");
    }
}

__declspec(noinline) DWORD WorkerWaitAtDepth(WorkerContext* context) {
    if (!SetEvent(context->deepestEntered)) return GetLastError();
    const DWORD result = WaitForSingleObject(context->releaseWorker, context->waitMilliseconds);
    if (result == WAIT_OBJECT_0) return ERROR_SUCCESS;
    return result == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
}

__declspec(noinline) DWORD WorkerMiddle(WorkerContext* context) {
    const DWORD result = WorkerWaitAtDepth(context);
    return result;
}

__declspec(noinline) DWORD WorkerOuter(WorkerContext* context) {
    const DWORD result = WorkerMiddle(context);
    return result;
}

__declspec(noinline) DWORD WINAPI WorkerEntry(void* parameter) {
    const DWORD result = WorkerOuter(static_cast<WorkerContext*>(parameter));
    return result;
}

int main(int argumentCount, char** arguments) {
    const bool automatic = argumentCount == 2 && std::strcmp(arguments[1], "--auto") == 0;
    if (argumentCount > 1 && !automatic) {
        std::fprintf(stderr, "Usage: start_address_and_stack.exe [--auto]\n");
        return 1;
    }

    NameThread(GetCurrentThread(), L"stack-main");
    std::printf("Phase 1: PID=%lu (0x%lx), main TID=%lu (0x%lx)\n",
                GetCurrentProcessId(), GetCurrentProcessId(), GetCurrentThreadId(), GetCurrentThreadId());
    std::printf("Logical user start WorkerEntry=%p\n", reinterpret_cast<void*>(&WorkerEntry));
    std::printf("WorkerOuter=%p, WorkerMiddle=%p, WorkerWaitAtDepth=%p\n",
                reinterpret_cast<void*>(&WorkerOuter), reinterpret_cast<void*>(&WorkerMiddle),
                reinterpret_cast<void*>(&WorkerWaitAtDepth));

    WorkerContext context{};
    context.waitMilliseconds = automatic ? 6000 : 120000;
    context.deepestEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(context.deepestEntered != nullptr, "CreateEventW(deepestEntered)");
    context.releaseWorker = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(context.releaseWorker != nullptr, "CreateEventW(releaseWorker)");

    DWORD workerId = 0;
    HANDLE worker = CreateThread(nullptr, 0, WorkerEntry, &context, 0, &workerId);
    Check(worker != nullptr, "CreateThread");
    NameThread(worker, L"stack-worker");
    std::printf("Worker TID=%lu (0x%lx)\n", workerId, workerId);
    WaitChecked(context.deepestEntered, 6000, "Wait for deepest helper entry");

    std::printf("Phase 2: deepest helper entered; worker is approaching or inside its event wait.\n");
    Checkpoint(automatic, "Compare the logical start address, reported start address, current RIP, and full stack.");

    std::printf("Phase 3: release the worker, join, and close handles.\n");
    Check(SetEvent(context.releaseWorker), "SetEvent(releaseWorker)");
    WaitChecked(worker, 6000, "Join worker");
    DWORD exitCode = 0;
    Check(GetExitCodeThread(worker, &exitCode), "GetExitCodeThread");
    Check(CloseHandle(worker), "CloseHandle(worker)");
    Check(CloseHandle(context.deepestEntered), "CloseHandle(deepestEntered)");
    Check(CloseHandle(context.releaseWorker), "CloseHandle(releaseWorker)");
    std::printf("Worker exit code=%lu; expected 0.\n", exitCode);
    return exitCode == ERROR_SUCCESS ? 0 : 1;
}