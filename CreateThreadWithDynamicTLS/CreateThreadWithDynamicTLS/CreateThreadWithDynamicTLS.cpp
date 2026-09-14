#include <stdio.h>
#include <windows.h>

#define NUM_THREADS     2
#define INITIAL_VALUE   -1

// Global TLS index variable
DWORD g_dwTlsIndex = 0x00;

// The WorkerThread function will be called by each created thread.
DWORD WINAPI WorkerThread(LPVOID lpParameter) {
    printf("[i] Thread [%d]: The TLS Index: %d\n", GetCurrentThreadId(), g_dwTlsIndex);

    // Retrieve the unique initial value of the TLS variable.
    int iValue = (int)lpParameter;

    // Retrieve the TLS value for this thread. If it hasn't been allocated yet, allocate it.
    int* iTLSVar = (int*)TlsGetValue(g_dwTlsIndex);

    if (!iTLSVar)
    {
        // Allocate memory for the TLS value.
        if (!(iTLSVar = (int*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(int)))) {
            printf("[!] HeapAlloc Failed With Error: %d\n", GetLastError());
            return -1;
        }

        // Set the TLS value for this thread.
        *iTLSVar = (int)INITIAL_VALUE;

        // Set the TLS value for this thread.
        if (!TlsSetValue(g_dwTlsIndex, iTLSVar)) {
            printf("[!] TlsSetValue Failed With Error: %d\n", GetLastError());
            return -1;
        }
    }

    printf("[i] Thread [%d]: The TLS Variable's Value: %d\n", GetCurrentThreadId(), *iTLSVar);


    // Initialize the thread-local variable uniquely for each thread.
    *iTLSVar = iValue;

    // Simulate some work: modify the TLS variable 
    for (int i = 0; i < 5; i++) {
        *iTLSVar += 10;
        // Sleep for 50ms to simulate work
        Sleep(50);
    }

    printf("[>] Thread [%d]: Updated The TLS Variable's Value: %d\n", GetCurrentThreadId(), *iTLSVar);

    // Free the memory allocated for the TLS variable.
    HeapFree(GetProcessHeap(), 0, iTLSVar);
    return 0;
}


int main() {

    HANDLE hThreads[NUM_THREADS] = { 0 };

    if ((g_dwTlsIndex = TlsAlloc()) == TLS_OUT_OF_INDEXES) {
        printf("[!] TlsAlloc Failed With Error: %d\n", GetLastError());
        return -1;
    }

    // Create two worker threads, and wait for each independently.
    for (int i = 0; i < NUM_THREADS; i++) {
        if (!(hThreads[i] = CreateThread(NULL, 0x00, WorkerThread, (LPVOID)(200 * i), 0x00, NULL))) {
            printf("[!] CreateThread Failed With Error: %d \n", GetLastError());
            return -1;
        }
        WaitForSingleObject(hThreads[i], INFINITE);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        if (hThreads[i])
            CloseHandle(hThreads[i]);
    }
    TlsSetValue(g_dwTlsIndex, NULL);
    TlsFree(g_dwTlsIndex);
    return 0;
}