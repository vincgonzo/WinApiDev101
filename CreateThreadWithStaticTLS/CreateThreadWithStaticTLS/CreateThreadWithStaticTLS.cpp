#include <stdio.h>
#include <windows.h>

#define NUM_THREADS     2

// Declare a static thread-local variable. Each thread will have its own instance of tlsVar.
__declspec(thread) static int g_iCounter = -1;


// The WorkerThread function will be called by each created thread.
DWORD WINAPI WorkerThread(LPVOID lpParameter) {

    // The parameter is passed as a special integer value.
    int iValue = (int)lpParameter;

    printf("[i] Thread [%d]: The TLS Variable's Value: %d\n", GetCurrentThreadId(), g_iCounter);

    // Initialize the thread-local variable uniquely for each thread.
    g_iCounter = iValue;

    // Simulate some work: modify g_iCounter 
    for (int i = 0; i < 5; i++) {
        g_iCounter += 10;
        // Sleep for 50ms to simulate work
        Sleep(50);
    }

    printf("[>] Thread [%d]: Updated The TLS Variable's Value: %d\n", GetCurrentThreadId(), g_iCounter);

    return 0;
}


int main() {

    HANDLE hThreads[NUM_THREADS] = { 0 };

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
    printf("[*] Main Thread: The TLS Variable's Value: %d\n", g_iCounter);
    return 0;
}
