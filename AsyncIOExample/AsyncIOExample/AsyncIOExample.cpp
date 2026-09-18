#include <windows.h>
#include <stdio.h>

#define CHUNK_SIZE 4096
#define GET_FILENAME(PATH) (strrchr(PATH, '\\') ? strrchr(PATH, '\\') + 1 : PATH)

typedef struct _BUFFER_ITEM {
    PBYTE pBuffer; // Ptr to chunk buffer
    DWORD dwBufferSize; // size of chunk buffer
    struct _BUFFER_ITEM* pNext; //PTR to nxt buffer item
} BUFFER_ITEM, *PBUFFER_ITEM;

typedef struct _BUFFER_QUEUE {
    CRITICAL_SECTION CriticalSection; // Critical section to protect the queue
    BUFFER_ITEM* pHead; // PTR to head of the queue
    BUFFER_ITEM* pTail; // PTR to end of the queue
    HANDLE hDataAvailable; // Event to signal data availability in the queue
    BOOL bFinished;
} BUFFER_QUEUE, *PBUFFER_QUEUE;

BUFFER_QUEUE pQueue = { 0 };

BOOL InitQueue(OUT PBUFFER_QUEUE pQueue) {
    InitializeCriticalSection(&pQueue->CriticalSection);
    pQueue->pHead = pQueue->pTail = NULL;

    if (!(pQueue->hDataAvailable = CreateEvent(NULL, TRUE, FALSE, NULL))) {
        printf("[!] CreateEvent [%d] Failed with Error: %lu\n", __LINE__, GetLastError());
        return FALSE;
    }

    pQueue->bFinished = FALSE;
    return TRUE;
}

void DestroyQueue(IN PBUFFER_QUEUE pQueue) {
    DeleteCriticalSection(&pQueue->CriticalSection);
    if (pQueue->hDataAvailable)
        CloseHandle(&pQueue->hDataAvailable);
}

BOOL EnqueueBuffer(IN OUT PBUFFER_QUEUE pQueue, IN PBYTE pBuffer, IN DWORD dwBufferSize) {
    PBUFFER_ITEM pNewItem = NULL;

    if (!(pNewItem = (PBUFFER_ITEM)LocalAlloc(LPTR, sizeof(BUFFER_ITEM)))) {
        printf("[!] LocalAlloc [%d] Failed with Error: %lu\n", __LINE__, GetLastError());
        return FALSE;
    }

    pNewItem->pBuffer = pBuffer;
    pNewItem->dwBufferSize = dwBufferSize;
    pNewItem->pNext = NULL;

    EnterCriticalSection(&pQueue->CriticalSection);
    if (pQueue->pTail) {
        pQueue->pTail->pNext = pNewItem;
        pQueue->pTail = pNewItem;
    }
    else {
        pQueue->pHead = pQueue->pTail = pNewItem;
    }

    if (!SetEvent(pQueue->hDataAvailable)) {
        printf("[!] SetEvent [%d] Failed with Error: %lu\n", __LINE__, GetLastError());
        LeaveCriticalSection(&pQueue->CriticalSection);
        LocalFree(pNewItem);
        return FALSE;
    }

    LeaveCriticalSection(&pQueue->CriticalSection);
    return TRUE;
}

BUFFER_ITEM* DequeueBuffer(IN PBUFFER_QUEUE pQueue) {
    BUFFER_ITEM* pQueueItem = NULL;

    EnterCriticalSection(&pQueue->CriticalSection);
    if (pQueue->pHead) {
        pQueueItem = pQueue->pHead;
        pQueue->pHead = pQueueItem->pNext;

        if (pQueue->pHead == NULL)
            pQueue->pTail = NULL;
        if (pQueue->pHead == NULL)
            ResetEvent(pQueue->hDataAvailable);
    }
    LeaveCriticalSection(&pQueue->CriticalSection);
    return pQueueItem;
}

BOOL WINAPI ReaderThread(LPVOID lpParameter) {
    LPCSTR lpSourceFilePath = (LPCSTR)lpParameter;
    HANDLE hSourceFile = INVALID_HANDLE_VALUE;
    OVERLAPPED OverlappedRead = { 0 };
    DWORD dwNumberOfBytesRead = 0x00,
           dwError = 0x00;
    ULARGE_INTEGER uliOffset = { 0 };
    LARGE_INTEGER liFileSize = { 0 };
    PBYTE pChunkBuffer = NULL;
    BOOL bResult = FALSE;

    if ((hSourceFile = CreateFileA(lpSourceFilePath, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL)) == INVALID_HANDLE_VALUE) {
        printf("[!] CreateFileA [%d] Failed with Error: %lu\n", __LINE__, GetLastError());
        goto _END_OF_FUNC;
    }

    if (!GetFileSizeEx(hSourceFile, &liFileSize)) {
        printf("[!] GetFileSizeEx [%d] Failed with Error: %lu\n", __LINE__, GetLastError());
        goto _END_OF_FUNC;
    }
    //Read Chunk by Chunk the file
    while (uliOffset.QuadPart < liFileSize.QuadPart) {
        if (!(pChunkBuffer = (PBYTE)LocalAlloc(LPTR, CHUNK_SIZE))) {
            printf("[!] LocalAlloc [%d] Failed with Error: %lu\n", __LINE__, GetLastError());
            goto _END_OF_FUNC;
        }
        //Set offset of next read operation
        OverlappedRead.Offset = uliOffset.LowPart;
        OverlappedRead.OffsetHigh = uliOffset.HighPart;
        // Start async read
        if (!ReadFile(hSourceFile, pChunkBuffer, CHUNK_SIZE, NULL, &OverlappedRead)) {
            if ((dwError = GetLastError()) != ERROR_IO_PENDING) {
                printf("[!] ReadFile [%d] Failed with Error: %lu\n", __LINE__, GetLastError());
                goto _END_OF_FUNC;
            }
        }
        // get the nbr of bytes read, waiting for the operation to complete
        if (!GetOverlappedResult(hSourceFile, &OverlappedRead, &dwNumberOfBytesRead, TRUE)) {
            printf("[!] GetOverlappedResult [%d] Failed with Error: %lu\n", __LINE__, GetLastError());
            goto _END_OF_FUNC;
        }

        if (dwNumberOfBytesRead == 0x00) {
            LocalFree(pChunkBuffer);
            break;
        }

        EnqueueBuffer(&pQueue, pChunkBuffer, dwNumberOfBytesRead);
        uliOffset.QuadPart += dwNumberOfBytesRead;
    }

    bResult = TRUE;

_END_OF_FUNC:
    if (!bResult && pChunkBuffer)
        LocalFree(pChunkBuffer);
    if (hSourceFile != INVALID_HANDLE_VALUE)
        CloseHandle(hSourceFile);
    EnterCriticalSection(&pQueue.CriticalSection);
    SetEvent(pQueue.hDataAvailable);
    LeaveCriticalSection(&pQueue.CriticalSection);
    pQueue.bFinished = TRUE;
    return bResult;
}

BOOL WINAPI WriterThread(LPVOID lpParameter) {
    LPCSTR lpDestFilePath = (LPCSTR)lpParameter;
    HANDLE hDestFile = INVALID_HANDLE_VALUE;
    OVERLAPPED OverlappedWrite = { 0 };
    DWORD dwNumberOfBytesWritten = 0x00,
        dwBufferSize = 0x00,
        dwError = 0x00;
    ULARGE_INTEGER uliOffset = { 0 };
    LARGE_INTEGER liFileSize = { 0 };
    PBYTE pChunkBuffer = NULL;
    BOOL bResult = FALSE;
    PBUFFER_ITEM pBufferItem = NULL;
    DWORD dwWaitResult = 0;

    if ((hDestFile = CreateFileA(lpDestFilePath, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_FLAG_OVERLAPPED, NULL)) == INVALID_HANDLE_VALUE) {
        printf("[!] CreateFileA [%d] Failed with Error: %lu\n", __LINE__, GetLastError());
        goto _END_OF_FUNC;
    }

    while(TRUE) {
        dwBufferSize = 0x00;
        dwWaitResult = WaitForSingleObject(pQueue.hDataAvailable, 1000); //1 sec wait

        EnterCriticalSection(&pQueue.CriticalSection);
        BOOL bFinished = pQueue.bFinished;
        LeaveCriticalSection(&pQueue.CriticalSection);

        if (dwWaitResult == WAIT_TIMEOUT && bFinished) {
            break;
        }
        if (dwWaitResult == WAIT_TIMEOUT)
            continue;

        if (!(pBufferItem = DequeueBuffer(&pQueue))) {
            EnterCriticalSection(&pQueue.CriticalSection);
            bFinished = pQueue.bFinished;
            LeaveCriticalSection(&pQueue.CriticalSection);

            if (bFinished)
                break;
            continue;
        }
        pChunkBuffer = pBufferItem->pBuffer;
        dwBufferSize = pBufferItem->dwBufferSize;

        LocalFree(pBufferItem);
        OverlappedWrite.Offset = uliOffset.LowPart;
        OverlappedWrite.OffsetHigh = uliOffset.HighPart;

        if (!WriteFile(hDestFile, pChunkBuffer, dwBufferSize
            , NULL, &OverlappedWrite)) {
            if ((dwError = GetLastError()) != ERROR_IO_PENDING) {
                printf("[!] WriteFile [%d] Failed with Error: %lu\n", __LINE__, GetLastError());
                goto _END_OF_FUNC;
            }
        }
        // Get the number of bytes written, waiting for the operation to complete.
        if (!GetOverlappedResult(hDestFile, &OverlappedWrite, &dwNumberOfBytesWritten, TRUE)) {
            printf("[!] GetOverlappedResult [%ld] Failed With Error: %lu\n", __LINE__, GetLastError());
            goto _END_OF_FUNC;
        }

        // Check if the number of bytes written matches the buffer size.
        if (dwNumberOfBytesWritten != dwBufferSize) {
            printf("[!] Mismatch in bytes [%ld]. Expected %lu, wrote %lu\n", __LINE__, dwBufferSize, dwNumberOfBytesWritten);
            goto _END_OF_FUNC;
        }

        // Update the file offset.
        uliOffset.QuadPart += dwNumberOfBytesWritten;

        LocalFree(pChunkBuffer);
    }

    bResult = TRUE;

_END_OF_FUNC:
    if (!bResult && pChunkBuffer != NULL)
        LocalFree(pChunkBuffer);
    if (hDestFile != INVALID_HANDLE_VALUE)
        CloseHandle(hDestFile);
    return bResult;
}

BOOL StartCopy(IN LPCSTR lpSourceFilePath, IN LPCSTR lpDestDir) {
    HANDLE hReaderThread = NULL,
        hWriteThread = NULL;
    CHAR cDestPath[MAX_PATH] = { 0 };
    BOOL bResult = FALSE;

    snprintf(cDestPath, MAX_PATH, "%s\\%s", lpDestDir, GET_FILENAME(lpSourceFilePath));
    InitQueue(&pQueue);

    if ((hReaderThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)ReaderThread, (LPVOID)lpSourceFilePath, 0, NULL)) == NULL) {
        printf("[!] CreateThread [%ld] Failed With Error: %lu\n", __LINE__, GetLastError());
        goto _END_OF_FUNC;
    }

    if ((hWriteThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)WriterThread, (LPVOID)cDestPath, 0, NULL)) == NULL) {
        printf("[!] CreateThread [%ld] Failed With Error: %lu\n", __LINE__, GetLastError());
        goto _END_OF_FUNC;
    }

    WaitForSingleObject(hReaderThread, INFINITE);
    WaitForSingleObject(hWriteThread, INFINITE);

#ifdef DEBUG
    printf("[*] Copy Completed Successfully.\n");
    printf("[*] Copied:\n\t[>] From: %s\n\t[>] To: %s\n", lpSourceFilePath, cDestPath);
#endif // DEBUG
    bResult = TRUE;

_END_OF_FUNC:
    if (hReaderThread)
        CloseHandle(hReaderThread);
    if (hWriteThread)
        CloseHandle(hWriteThread);
    DestroyQueue(&pQueue);
    return bResult;
}

int main(int argc, char* argv[])
{
    LARGE_INTEGER Frequency = { 0 },
        ExecStart = { 0 },
        ExecEnd = { 0 };
    DOUBLE ExecTime = 0.0;

    if (argc < 3) {
        printf("[!] Usage: %s <source file> <destination directory>\n", argv[0]);
        return -1;
    }

    if (!QueryPerformanceFrequency(&Frequency)) {
        printf("[!] QueryPerformanceFrequency Failed With Error: %lu\n", GetLastError());
        return -1;
    }

    QueryPerformanceCounter(&ExecStart);

    if (!StartCopy(argv[1], argv[2])) {
        printf("[!] Copy Operation Failed\n");
        return -1;
    }
    QueryPerformanceCounter(&ExecEnd);
    ExecTime = (DOUBLE)(ExecEnd.QuadPart - ExecStart.QuadPart) / Frequency.QuadPart;
    printf("[*] Execution Time: %f Seconds\n", ExecTime);
    return 0;
}