// CreateThreadSafeFunction.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <windows.h>
#include <stdio.h>

#define THREAD_COUNT 10
#define ITERATIONS 1000000

LONG g_lcounter = 0;
CRITICAL_SECTION g_CriticalSection = { 0 };

DWORD WINAPI ThreadProc(LPVOID lpParam) {
    for (int i = 0; i < ITERATIONS; i++) {
        EnterCriticalSection(&g_CriticalSection); // Lock

        g_lcounter++;

        LeaveCriticalSection(&g_CriticalSection); // Unlock
    }
    return 0;
}

int main()
{
    printf("[i] Using classic ++ Operator\n");

    HANDLE hThreads[THREAD_COUNT] = { 0 };
    DWORD dwThreadId = 0x00;

    // Necessary to init Critical Section before imagining using it.
    InitializeCriticalSection(&g_CriticalSection);

    for (int i = 0; i < THREAD_COUNT; i++) {
        if ((hThreads[i] = CreateThread(NULL, 0, ThreadProc, NULL, 0, &dwThreadId)) == NULL) {
            printf("[!] CreateThread [%d] Failed With Error: %d\n", i, GetLastError());
            return -1;
        }
        printf("[!] Thread %0.2 Created Successfully With ID: %d\n", i, dwThreadId);
    }

    WaitForMultipleObjects(THREAD_COUNT, hThreads, TRUE, INFINITE);

    for (int i = 0; i < THREAD_COUNT; i++) {
        if (hThreads[i] != NULL) CloseHandle(hThreads[i]);
    }

    // & also to liberate the Critical Section before closing program.
    DeleteCriticalSection(&g_CriticalSection);

    printf("[*] Final Counter: %d\n", g_lcounter);
    return 0;
}
