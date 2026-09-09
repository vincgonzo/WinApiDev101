// CreateThreadSafeFunction.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <windows.h>
#include <stdio.h>

#define THREAD_COUNT 10
#define ITERATIONS 1000000

LONG g_lcounter = 0;

DWORD WINAPI ThreadProc(LPVOID lpParam) {
    for (int i = 0; i < ITERATIONS; i++) {
        InterlockedIncrement(&g_lcounter);
    }
    return 0;
}

int main()
{
    printf("[i] Using Thread-Safe Atomic Operation\n");

    HANDLE hThreads[THREAD_COUNT] = { 0 };
    DWORD dwThreadId = 0x00;

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

    printf("[*] Final Counter: %d\n", g_lcounter);
    return 0;
}
