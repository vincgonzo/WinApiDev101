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

void InspectionCheckpoint(bool automatic) {
    if (automatic) {
        std::puts("Inspection checkpoint: --auto continues immediately.");
        return;
    }
    std::puts("Inspect the live stacks and TLS now. Press Enter, or wait 30 seconds.");
    std::fflush(stdout);
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (input == nullptr || input == INVALID_HANDLE_VALUE || !GetConsoleMode(input, &mode)) {
        std::puts("No interactive console input; skipping the inspection checkpoint.");
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

struct CapturedState {
    DWORD threadId = 0;
    const void* teb = nullptr;
    const void* stackBase = nullptr;
    const void* stackLimit = nullptr;
    const LONG* localAddress = nullptr;
    const DWORD* tlsAddress = nullptr;
    LONG localValue = 0;
    DWORD tlsValue = 0;
    DWORD tlsReadLastError = 0;
    DWORD immediateLastError = 0;
};

struct WorkerContext {
    HANDLE prepared = nullptr;
    HANDLE ready = nullptr;
    HANDLE start = nullptr;
    HANDLE release = nullptr;
    DWORD tlsIndex = TLS_OUT_OF_INDEXES;
    DWORD holdTimeout = 0;
    DWORD slot = 0;
    volatile LONG* sharedCounter = nullptr;
    CapturedState captured{};
};

constexpr LONG incrementsPerWorker = 100000;

__declspec(noinline) unsigned __stdcall SharedStateWorker(void* argument) {
    auto& context = *static_cast<WorkerContext*>(argument);
    LONG localValue = 1000 + static_cast<LONG>(context.slot);
    DWORD tlsValue = 0xA1100000UL + context.slot;
    Check(TlsSetValue(context.tlsIndex, &tlsValue), "TlsSetValue(worker storage)");
    void* tlsPointer = TlsGetValue(context.tlsIndex);
    const DWORD tlsReadLastError = GetLastError();
    if (tlsPointer == nullptr && tlsReadLastError != ERROR_SUCCESS) {
        Fail("TlsGetValue", tlsReadLastError);
    }
    Verify(tlsPointer == &tlsValue, "worker reads its own TLS slot");

    const auto* teb = NtCurrentTeb();
    const auto* tib = reinterpret_cast<const NT_TIB*>(teb);
    auto& captured = context.captured;
    captured.threadId = GetCurrentThreadId();
    captured.teb = teb;
    captured.stackBase = tib->StackBase;
    captured.stackLimit = tib->StackLimit;
    captured.localAddress = &localValue;
    captured.tlsAddress = static_cast<const DWORD*>(tlsPointer);
    captured.localValue = localValue;
    captured.tlsValue = *captured.tlsAddress;
    captured.tlsReadLastError = tlsReadLastError;

    SetLastError(0xE1000000UL + context.slot);
    const DWORD immediateLastError = GetLastError();
    captured.immediateLastError = immediateLastError;

    Check(SetEvent(context.prepared), "SetEvent(prepared)");
    WaitOne(context.start, 5000, "WaitForSingleObject(start)");
    for (LONG iteration = 0; iteration < incrementsPerWorker; ++iteration) {
        InterlockedIncrement(context.sharedCounter);
    }
    Check(SetEvent(context.ready), "SetEvent(ready)");
    WaitOne(context.release, context.holdTimeout, "WaitForSingleObject(release)");
    Verify(localValue == captured.localValue && tlsValue == captured.tlsValue,
           "worker-local storage is unchanged through inspection");
    Check(TlsSetValue(context.tlsIndex, nullptr), "TlsSetValue(clear)");
    return 0;
}

int main(int argumentCount, char** arguments) {
    const bool automatic = argumentCount == 2 && std::strcmp(arguments[1], "--auto") == 0;
    if (argumentCount != 1 && !automatic) {
        std::fprintf(stderr, "Usage: shared_state_and_tls.exe [--auto]\n");
        return 1;
    }
    NameThread(GetCurrentThread(), L"shared-state-coordinator");
    const DWORD processId = GetCurrentProcessId();
    const DWORD mainId = GetCurrentThreadId();
    std::printf("PID=%lu (0x%lx), main TID=%lu (0x%lx)\n", processId, processId, mainId, mainId);

    const DWORD tlsIndex = TlsAlloc();
    if (tlsIndex == TLS_OUT_OF_INDEXES) {
        const DWORD error = GetLastError();
        Fail("TlsAlloc", error);
    }
    HANDLE start = NewEvent();
    HANDLE release = NewEvent();
    volatile LONG sharedCounter = 0;
    WorkerContext contexts[2]{};
    HANDLE threads[2]{};
    HANDLE prepared[2]{};
    HANDLE ready[2]{};
    unsigned threadIds[2]{};
    const wchar_t* descriptions[2] = {L"shared-state-A", L"shared-state-B"};

    for (DWORD slot = 0; slot < 2; ++slot) {
        auto& context = contexts[slot];
        context.prepared = NewEvent();
        context.ready = NewEvent();
        context.start = start;
        context.release = release;
        context.tlsIndex = tlsIndex;
        context.holdTimeout = automatic ? 8000 : 45000;
        context.slot = slot;
        context.sharedCounter = &sharedCounter;
        prepared[slot] = context.prepared;
        ready[slot] = context.ready;
        const uintptr_t rawHandle = _beginthreadex(nullptr, 0, SharedStateWorker, &context, 0,
                                                  &threadIds[slot]);
        if (rawHandle == 0) {
            const int error = errno;
            std::fprintf(stderr, "_beginthreadex failed: errno=%d\n", error);
            ExitProcess(1);
        }
        threads[slot] = reinterpret_cast<HANDLE>(rawHandle);
        NameThread(threads[slot], descriptions[slot]);
    }

    WaitBoth(prepared, 3000, "WaitForMultipleObjects(prepared)");
    Check(SetEvent(start), "SetEvent(start)");
    WaitBoth(ready, 5000, "WaitForMultipleObjects(ready)");
    const LONG total = InterlockedCompareExchange(&sharedCounter, 0, 0);
    std::printf("Shared counter at %p: %ld, expected %ld; one TLS index=%lu\n",
                const_cast<LONG*>(&sharedCounter), total, 2 * incrementsPerWorker, tlsIndex);
    Verify(total == 2 * incrementsPerWorker, "both workers contributed every increment");

    for (DWORD slot = 0; slot < 2; ++slot) {
        const auto& captured = contexts[slot].captured;
        std::printf("Worker %c TID=%lu (0x%lx), TEB=%p, stack=[%p, %p)\n",
                    static_cast<int>('A' + slot), captured.threadId, captured.threadId,
                    captured.teb, captured.stackLimit, captured.stackBase);
        std::printf("  local=%ld at %p; TLS value=0x%08lx at %p\n",
                    captured.localValue, static_cast<const void*>(captured.localAddress),
                    captured.tlsValue, static_cast<const void*>(captured.tlsAddress));
        std::printf("  TlsGetValue immediate last-error=%lu; SetLastError immediate capture=0x%08lx\n",
                    captured.tlsReadLastError, captured.immediateLastError);
        const uintptr_t localAddress = reinterpret_cast<uintptr_t>(captured.localAddress);
        const uintptr_t stackLimit = reinterpret_cast<uintptr_t>(captured.stackLimit);
        const uintptr_t stackBase = reinterpret_cast<uintptr_t>(captured.stackBase);
        Verify(captured.threadId == threadIds[slot], "captured and creation TIDs match");
        Verify(localAddress >= stackLimit && localAddress < stackBase, "local is within NT_TIB stack bounds");
        Verify(captured.localValue == 1000 + static_cast<LONG>(slot), "expected local value");
        Verify(*captured.localAddress == captured.localValue, "main can read the live worker stack");
        Verify(captured.tlsValue == 0xA1100000UL + slot, "expected per-thread TLS value");
        Verify(*captured.tlsAddress == captured.tlsValue, "main can read the live TLS payload");
        Verify(captured.tlsReadLastError == ERROR_SUCCESS, "successful TlsGetValue cleared last-error");
        Verify(captured.immediateLastError == 0xE1000000UL + slot, "per-thread immediate last-error capture");
    }
    Verify(contexts[0].captured.teb != contexts[1].captured.teb, "different TEBs");
    Verify(contexts[0].captured.localAddress != contexts[1].captured.localAddress, "different stack locals");
    Verify(contexts[0].captured.tlsAddress != contexts[1].captured.tlsAddress, "different TLS payloads");
    std::puts("Captured values are published. Both workers are approaching or inside their release wait.");
    InspectionCheckpoint(automatic);

    Check(SetEvent(release), "SetEvent(release)");
    WaitBoth(threads, 3000, "WaitForMultipleObjects(join)");
    for (DWORD slot = 0; slot < 2; ++slot) {
        DWORD exitCode = 0;
        Check(GetExitCodeThread(threads[slot], &exitCode), "GetExitCodeThread");
        Verify(exitCode == 0, "worker exit code is zero");
        Check(CloseHandle(threads[slot]), "CloseHandle(thread)");
        Check(CloseHandle(prepared[slot]), "CloseHandle(prepared)");
        Check(CloseHandle(ready[slot]), "CloseHandle(ready)");
    }
    Check(TlsFree(tlsIndex), "TlsFree");
    Check(CloseHandle(start), "CloseHandle(start)");
    Check(CloseHandle(release), "CloseHandle(release)");
    std::puts("PASS: shared counter, live distinct stacks/TLS, and immediate last-error captures verified.");
    return 0;
}