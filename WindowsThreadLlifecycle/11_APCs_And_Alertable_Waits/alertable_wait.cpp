#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>

struct ApcContext {
    HANDLE enteredEvent;
    HANDLE releaseGate;
    DWORD gateTimeout;
    DWORD entryThreadId;
    DWORD sleepResult;
    ULONGLONG sleepElapsed;
    LONG callbackCount;
    LONG callbackThreadId;
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

void WaitChecked(HANDLE handle, const char* operation) {
    const DWORD result = WaitForSingleObject(handle, 6000);
    if (result != WAIT_OBJECT_0) {
        Fail(operation, result == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT);
    }
}

__declspec(noinline) VOID CALLBACK BenignApc(ULONG_PTR parameter) {
    auto* context = reinterpret_cast<ApcContext*>(parameter);
    InterlockedExchange(&context->callbackThreadId, static_cast<LONG>(GetCurrentThreadId()));
    InterlockedIncrement(&context->callbackCount);
}

__declspec(noinline) DWORD WINAPI ApcTarget(void* parameter) {
    auto* context = static_cast<ApcContext*>(parameter);
    context->entryThreadId = GetCurrentThreadId();
    if (!SetEvent(context->enteredEvent)) return GetLastError();
    const DWORD gateResult = WaitForSingleObject(context->releaseGate, context->gateTimeout);
    if (gateResult != WAIT_OBJECT_0) {
        return gateResult == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
    }
    const ULONGLONG started = GetTickCount64();
    context->sleepResult = SleepEx(2000, TRUE);
    context->sleepElapsed = GetTickCount64() - started;
    return ERROR_SUCCESS;
}

int main(int argumentCount, char** arguments) {
    const bool automatic = argumentCount == 2 && std::strcmp(arguments[1], "--auto") == 0;
    if (argumentCount > 1 && !automatic) {
        std::fprintf(stderr, "Usage: alertable_wait.exe [--auto]\n");
        return 1;
    }

    NameThread(GetCurrentThread(), L"alertable-main");
    const DWORD mainId = GetCurrentThreadId();
    std::printf("Phase 1: PID=%lu (0x%lx), queuing main TID=%lu (0x%lx)\n",
                GetCurrentProcessId(), GetCurrentProcessId(), mainId, mainId);
    ApcContext context{};
    context.gateTimeout = automatic ? 6000 : 120000;
    context.sleepResult = MAXDWORD;
    context.enteredEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(context.enteredEvent != nullptr, "CreateEventW(entered)");
    context.releaseGate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(context.releaseGate != nullptr, "CreateEventW(release)");
    DWORD workerId = 0;
    HANDLE worker = CreateThread(nullptr, 0, ApcTarget, &context, 0, &workerId);
    Check(worker != nullptr, "CreateThread");
    NameThread(worker, L"alertable-target");
    WaitChecked(context.enteredEvent, "Wait for actual worker entry");
    std::printf("Target TID=%lu (0x%lx); its entry event was observed before queuing.\n", workerId, workerId);

    Check(QueueUserAPC(BenignApc, worker, reinterpret_cast<ULONG_PTR>(&context)) != 0, "QueueUserAPC");
    const LONG beforeRelease = InterlockedCompareExchange(&context.callbackCount, 0, 0);
    std::printf("Phase 2: one ordinary user APC queued; callback count at non-alertable gate=%ld.\n", beforeRelease);
    if (!automatic) {
        std::printf("Set a breakpoint on BenignApc or inspect the target; press Enter to release the gate.\n");
        if (std::getchar() == EOF) std::printf("Console input ended; releasing target.\n");
    }
    Check(SetEvent(context.releaseGate), "SetEvent(release)");
    WaitChecked(worker, "Join target after SleepEx(TRUE)");

    DWORD exitCode = 0;
    Check(GetExitCodeThread(worker, &exitCode), "GetExitCodeThread");
    const LONG count = InterlockedCompareExchange(&context.callbackCount, 0, 0);
    const DWORD callbackId = static_cast<DWORD>(InterlockedCompareExchange(&context.callbackThreadId, 0, 0));
    std::printf("Phase 3: SleepEx(TRUE)=0x%lx (WAIT_IO_COMPLETION=0x%lx), elapsed=%llu ms.\n",
                context.sleepResult, static_cast<DWORD>(WAIT_IO_COMPLETION),
                static_cast<unsigned long long>(context.sleepElapsed));
    std::printf("Callback count=%ld, callback TID=%lu (0x%lx), target TID=%lu (0x%lx), caller TID=%lu (0x%lx).\n",
                count, callbackId, callbackId, workerId, workerId, mainId, mainId);
    const bool valid = beforeRelease == 0 && context.entryThreadId == workerId && exitCode == ERROR_SUCCESS
                       && context.sleepResult == WAIT_IO_COMPLETION && count == 1
                       && callbackId == workerId && callbackId != mainId;
    Check(CloseHandle(worker), "CloseHandle(worker)");
    Check(CloseHandle(context.enteredEvent), "CloseHandle(entered)");
    Check(CloseHandle(context.releaseGate), "CloseHandle(release)");
    std::printf("Exactly one APC executed on the existing target thread: %s.\n", valid ? "PASS" : "FAIL");
    return valid ? 0 : 1;
}