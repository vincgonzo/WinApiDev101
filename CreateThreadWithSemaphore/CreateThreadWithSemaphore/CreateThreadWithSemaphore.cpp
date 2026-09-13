#include <Windows.h>
#include <stdio.h>

#define BUFFER_SIZE     5                               // Size of the buffer.
#define PRODUCE_ITEMS   20                              // Number of items to produce/consume.

INT                 g_Buffer[BUFFER_SIZE] = { 0 };    // Buffer to store items.
INT                 g_In = 0x00;     // Input and Output indices. Used to add and remove items from the g_Buffer.
INT                 g_Out = 0x00;
HANDLE              g_hEmptySemaphore = NULL;     // Counts empty slots (initialized to BUFFER_SIZE)
HANDLE              g_hFullSemaphore = NULL;     // Counts full slots (initialized to 0)
CRITICAL_SECTION    g_CriticalSection = { 0 };    // Critical section to protect g_Buffer access.


DWORD WINAPI ProducerThread(LPVOID lpParam)
{
    for (int i = 0; i < PRODUCE_ITEMS; i++)
    {
        // Wait for an empty slot in the g_Buffer.
        WaitForSingleObject(g_hEmptySemaphore, INFINITE);

        // Enter critical section to add an item.
        EnterCriticalSection(&g_CriticalSection);
        // Produce an item (for simplicity, the item is just an integer).
        g_Buffer[g_In] = i + 1;

        printf("[i] Produced A New Item [%0.2d] At Index: %0.2d\n", g_Buffer[g_In], g_In);
        // Move to the next write slot (circulating the buffer).
        g_In = (g_In + 1) % BUFFER_SIZE;
        // Leave the critical section.
        LeaveCriticalSection(&g_CriticalSection);
        // Signal that a new item is available (increment g_hFullSemaphore).
        ReleaseSemaphore(g_hFullSemaphore, 0x01, NULL);

        // Simulate production delay.
        Sleep(50);
    }
    return 0;
}


DWORD WINAPI ConsumerThread(LPVOID lpParam)
{
    for (int i = 0; i < PRODUCE_ITEMS; i++)
    {
        // Wait for an item to be available in the g_Buffer.
        WaitForSingleObject(g_hFullSemaphore, INFINITE);

        // Enter critical section to remove an item.
        EnterCriticalSection(&g_CriticalSection);

        printf("\t[>] Consumed Item [%0.2d] From Index: %0.2d\n", g_Buffer[g_Out], g_Out);

        // Move to the next read slot (circulating the buffer).
        g_Out = (g_Out + 1) % BUFFER_SIZE;

        // Leave the critical section.
        LeaveCriticalSection(&g_CriticalSection);

        // Signal that an empty slot is available (increment g_hEmptySemaphore).
        ReleaseSemaphore(g_hEmptySemaphore, 0x01, NULL);

        // Simulate consumption delay.
        Sleep(100);
    }
    return 0;
}


int main() {

    INT     iReturn = -1;
    HANDLE  hProducer = NULL,
        hConsumer = NULL;

    // Initialize the critical section.
    InitializeCriticalSection(&g_CriticalSection);

    // Create semaphore 'g_hEmptySemaphore' that starts with BUFFER_SIZE (all slots are empty).
    if (!(g_hEmptySemaphore = CreateSemaphore(NULL, BUFFER_SIZE, BUFFER_SIZE, NULL))) {
        printf("[!] CreateSemaphore [E] Failed With Error: %ld\n", GetLastError());
        goto _END_OF_FUNC;
    }

    // Create semaphore 'g_hFullSemaphore' that starts with 0 (no items produced yet).
    if (!(g_hFullSemaphore = CreateSemaphore(NULL, 0, BUFFER_SIZE, NULL))) {
        printf("[!] CreateSemaphore [F] Failed With Error: %ld\n", GetLastError());
        goto _END_OF_FUNC;
    }

    // Create the producer thread.
    if (!(hProducer = CreateThread(NULL, 0, ProducerThread, NULL, 0, NULL))) {
        printf("[!] CreateThread [P] Failed With Error: %ld\n", GetLastError());
        goto _END_OF_FUNC;
    }

    // Create the consumer thread.
    if (!(hConsumer = CreateThread(NULL, 0, ConsumerThread, NULL, 0, NULL))) {
        printf("[!] CreateThread [C] Failed With Error: %ld\n", GetLastError());
        goto _END_OF_FUNC;
    }

    // Wait for both threads to finish.
    WaitForSingleObject(hProducer, INFINITE);
    WaitForSingleObject(hConsumer, INFINITE);

    iReturn = 0;

_END_OF_FUNC:
    if (hProducer)
        CloseHandle(hProducer);
    if (hConsumer)
        CloseHandle(hConsumer);
    if (g_hEmptySemaphore)
        CloseHandle(g_hEmptySemaphore);
    if (g_hFullSemaphore)
        CloseHandle(g_hFullSemaphore);
    DeleteCriticalSection(&g_CriticalSection);
    return iReturn;
}
