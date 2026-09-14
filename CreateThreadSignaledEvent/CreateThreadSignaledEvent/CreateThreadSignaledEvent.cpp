// CreateThreadSignaledEvent.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <windows.h>
#include <stdio.h>

#define EVENT_NAME L"MyEvent"

DWORD WINAPI WaiterThread(LPVOID lpParam) {
    HANDLE hEvent = NULL;

    if (!(hEvent = OpenEvent(EVENT_ALL_ACCESS, FALSE, EVENT_NAME))) {
        printf("[!] From The Waiter Thread: OpenEvent Failed With Error: %d\n", GetLastError());
        return 0;
    }

    printf("[i] From The Waiter Thread: Waiting For The Event To Be Signaled...\n");
    // Wait indefinitely until the event is signaled.
    if (WaitForSingleObject(hEvent, INFINITE) == WAIT_OBJECT_0)
        printf("[+] From The Waiter Thread: The Event Was Signaled!\n");
    else
    {
        printf("[!] From The Waiter Thread: WaitForSingleObject Failed With Error: %d\n", GetLastError());
        return 0;
    }
    return 1;
}

int main()
{
    INT iReturn = -1;
    HANDLE hEvent = NULL,
        hThread = NULL;// Create a manual-reset event: The event is initially non-signaled (FALSE), meaning threads waiting on it will block.
    if (!(hEvent = CreateEvent(NULL, TRUE, FALSE, EVENT_NAME))) {
        printf("[!] CreateEvent Failed With Error: %d\n", GetLastError());
        goto _END_OF_FUNC;
    }

    if (!(hThread = CreateThread(NULL, 0x00, WaiterThread, NULL, 0x00, NULL))) {
        printf("[!] CreateThread Failed With Error: %d\n", GetLastError());
        goto _END_OF_FUNC;
    }

    // Simulate work in the main thread before signaling the event.
    printf("[i] Main Thread: Doing Work ... \n");
    Sleep(5 * 255);
    printf("[i] Main Thread: Done Working, Signaling The Event ... \n");

    // Signal the event so that the waiter thread can continue.
    if (!SetEvent(hEvent)) {
        printf("[!] SetEvent Failed With Error: %d\n", GetLastError());
        goto _END_OF_FUNC;
    }

    // Wait for the waiter thread to finish.
    WaitForSingleObject(hThread, INFINITE);
    printf("[i] Main Thread: Finished Processing\n");

    iReturn = 0x00;




_END_OF_FUNC:
    if(hThread)
        CloseHandle(hThread);
    if(hEvent)
        CloseHandle(hEvent);
    return iReturn;
}