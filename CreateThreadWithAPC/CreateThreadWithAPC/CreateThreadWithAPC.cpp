#include <Windows.h>
#include <stdio.h>

// Global event handle.
HANDLE g_hEvent = NULL;

// APC callback function that will be executed in the context of the worker thread.
VOID CALLBACK CallbackFunctionAPC(ULONG_PTR dwParam) {
    printf("[*] APC Callback: Function Is Executed In Thread [%lu] With Parameter: %lu\n", GetCurrentThreadId(), (unsigned long)dwParam);
    // Simulate some work
    Sleep(500);
}

// Worker thread function that waits for an event to be signaled.
DWORD WINAPI WorkerThread(LPVOID lpParam) {
    printf("[>] Thread [%d] Started Working ... ", GetCurrentThreadId());
    // Simulate some work
    Sleep(500);
    printf("[+] DONE \n");
    printf("[>] Thread [%d] Entering Alertable Wait State ...\n", GetCurrentThreadId());

    // Loop until the event is signaled.
    while (1) {
        // Wait for the event for up to 1000 milliseconds in an alertable state (TRUE).
        DWORD dwResult = WaitForSingleObjectEx(g_hEvent, 1 * 1000, TRUE);

        switch (dwResult) {
            case WAIT_OBJECT_0: {
                printf("[>] Thread [%d] Detected Event Signal, Exiting\n", GetCurrentThreadId());
                return 0;
            }
            case WAIT_IO_COMPLETION: {
                printf("[>] Thread [%d] Detected An APC, Should Be Executed\n", GetCurrentThreadId());
                break;
            }
            case WAIT_TIMEOUT: {
                printf("[>] Thread [%d] Still Waiting In Alertable State ...\n", GetCurrentThreadId());
                break;
            }
            default: {
                printf("[>] Thread [%d] Encountered An Error: %lu\n", GetCurrentThreadId(), GetLastError());
                return -1;
            }
        }

        Sleep(500);
    }

    return 0;
}

int main() {

    HANDLE  hThread = NULL;
    DWORD   dwThreadId = 0x00;

    // Create an auto-reset event that is initially non-signaled.
    if (!(g_hEvent = CreateEvent(NULL, FALSE, FALSE, NULL))) {
        printf("[!] CreateEvent Failed With Error: %lu\n", GetLastError());
        return -1;
    }

    if (!(hThread = CreateThread(NULL, 0x00, WorkerThread, NULL, 0x00, &dwThreadId))) {
        printf("[!] CreateThread Failed With Error: %lu\n", GetLastError());
        return -1;
    }

    // Give the worker thread some time to enter its alertable wait state and to simulate waiting.
    Sleep(3 * 1000);

    // Queue an APC to the worker thread.
    if (!QueueUserAPC(CallbackFunctionAPC, hThread, 12345)) {
        printf("[!] QueueUserAPC Failed With Error: %lu\n", GetLastError());
        return -1;
    }
    printf("[i] Main Thread: APC Queued To Thread [%lu]\n", dwThreadId);
    // Wait a bit to ensure the APC is delivered.
    Sleep(1 * 1000);
    SetEvent(g_hEvent);
    WaitForSingleObject(hThread, INFINITE);
    printf("[*] Main Thread: Worker Thread Has Finished\n");
    CloseHandle(hThread);
    CloseHandle(g_hEvent);
    return 0;
}