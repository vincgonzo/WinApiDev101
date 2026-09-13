
#include <windows.h>
#include <stdio.h>

#pragma comment(lib, "Synchronization.lib")

#define THREAD_COUNT 3
#define ITERATIONS 16
#define SLEEP_VAL 50

LONG g_Counter = 0x00;

DWORD WINAPI WriterThread(LPVOID lpParameter) {
    for (int i = 0; i < ITERATIONS / 2; i++) {
        printf("\t[>] Writer: %d\n", g_Counter++);
        WakeByAddressAll((PVOID)(&g_Counter));
        Sleep(SLEEP_VAL);
    }
    return 0;
}

DWORD WINAPI WaiterThread(LPVOID lpParameter) {
    LONG LocalCounter = g_Counter;

    while (LocalCounter < ITERATIONS) {
        /*
        WaitOnAddress will check if the value at &g_Counter is still equal to LocalCounter.
        If it is, the thread sleeps until another thread changes *g_Counter and calls WakeByAddressAll.
        */
        WaitOnAddress((volatile VOID*) &g_Counter, &LocalCounter, sizeof(LONG), INFINITE);
        LocalCounter = g_Counter;
        printf("[*] Waiter: Fetch the counter of value: %d\n", g_Counter);
    }
    printf("[i] Waiter: Done Waiting;\n");
    return 0;
}


int main()
{
    printf("[i] Using WaitOnAddress And WakeByAddressAll APIs\n");
    HANDLE  hThreads[THREAD_COUNT] = { 0 };

    if (!(hThreads[0] = CreateThread(NULL, 0, WriterThread, NULL, 0, NULL))) {
        printf("[!] CreateThread [hWriter] Failed With Error: %d\n", GetLastError());
        return -1;
    }

    if (!(hThreads[1] = CreateThread(NULL, 0, WriterThread, NULL, 0, NULL))) {
        printf("[!] CreateThread [hWriter] Failed With Error: %d\n", GetLastError());
        return -1;
    }

    if (!(hThreads[2] = CreateThread(NULL, 0, WaiterThread, NULL, 0, NULL))) {
        printf("[!] CreateThread [hWaiter] Failed With Error: %d\n", GetLastError());
        return -1;
    }

    WaitForMultipleObjects(THREAD_COUNT, hThreads, TRUE, INFINITE);

    for (int i = 0; i < THREAD_COUNT; i++) {
        if (hThreads[i] != NULL) CloseHandle(hThreads[i]);
    }

    printf("[*] Final Counter Value: %ld\n", g_Counter);

    return 0;
}