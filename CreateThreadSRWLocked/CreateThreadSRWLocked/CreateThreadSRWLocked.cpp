// CreateThreadSafeFunction.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <windows.h>
#include <stdio.h>

#define THREAD_COUNT 10 + 1
#define ITERATIONS 1000000

LONG g_lCounter = 0;
SRWLOCK g_SRWLock = { 0 };


DWORD WINAPI ReaderThreadProc(LPVOID lpParam) {
    for (int i = 0; i < 10; i++) {
        AcquireSRWLockShared(&g_SRWLock); // Shared Read / Restrict write
        printf("[*][%0.2d] Reader Thread Fetched the Counter Of Value: %d\n", i + 1, g_lCounter);
        ReleaseSRWLockShared(&g_SRWLock);
        Sleep(32);
    }
    return 0;
}

DWORD WINAPI ThreadProc(LPVOID lpParam) {
    for (int i = 0; i < ITERATIONS; i++) {
        AcquireSRWLockExclusive(&g_SRWLock);
        g_lCounter++;
        ReleaseSRWLockExclusive(&g_SRWLock);

    }
    return 0;
}

int main()
{
    printf("[i] Using SRW Lock\n");

    HANDLE hThreads[THREAD_COUNT] = { 0 };
    DWORD dwThreadId = 0x00;

    // Like Critical Section this should be init
    InitializeSRWLock(&g_SRWLock);

    if ((hThreads[0] = CreateThread(NULL, 0, ReaderThreadProc, NULL, 0, &dwThreadId)) == NULL) {
        printf("[!] CreateThread [0] Failed With Error: %d\n", GetLastError());
        return -1;
    }

    printf("[+] Reader Thread Created Successfully With ID: %d\n", dwThreadId);

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

    // but no need to releae afterward

    printf("[*] Final Counter: %d\n", g_lCounter);
    return 0;
}
