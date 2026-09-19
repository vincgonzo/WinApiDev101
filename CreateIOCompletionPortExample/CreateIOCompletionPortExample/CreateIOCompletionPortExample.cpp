#include <windows.h>
#include <stdio.h>

#define CHUNK_SIZE 4096
// macro used to get the filename from a full path
#define GET_FILENAME(PATH) (strrchr(PATH, '\\') ? strrchr(PATH, '\\') + 1 : PATH)

volatile LONG64 g_OpCount = 0x00;
volatile HANDLE g_hCompletionEvent = NULL;
volatile HANDLE g_hICOP = NULL;
volatile HANDLE g_hworkerThread = NULL;

typedef enum _IO_OPERATION {
    IO_READ,
    IO_WRITE
} IO_OPERATION;

typedef struct _IO_DATA {
    OVERLAPPED Overlapped;
    ULONGLONG ullFileSize;
    PBYTE pChunkBuffer;
    HANDLE hSourceFile;
    HANDLE hDestFile;
    IO_OPERATION IoOperation;
    DWORD dwBytesTransferred;
} IO_DATA, *PIO_DATA;

BOOL WINAPI WorkerThreadProc(LPVOID lpParameter) {
    HANDLE hIOCP = (HANDLE)lpParameter;
    DWORD dwBytesTransferred = 0x00,
        dwError = 0x00;
    ULONG_PTR uCompletionKey = NULL;
    LPOVERLAPPED lpOverlapped = NULL;
    PIO_DATA pIoData = NULL,
        pNewIoData = NULL;
    ULARGE_INTEGER ulOffset = { 0 };
    BOOL bResult = FALSE;

    while (TRUE) {
        // wait for a completion notif from IOCP
        bResult = GetQueuedCompletionStatus(hIOCP, &dwBytesTransferred, &uCompletionKey, &lpOverlapped, INFINITE);

        if (lpOverlapped == NULL) // null lpOverlapped indicates signal (sent via PostQueueCompletionStatus)
            break;
        // retrieve IO_DATA struct corresponding to the completed operation.
        pIoData = (PIO_DATA)lpOverlapped;
        //if GetQueuedCompletionStatus return FALSE IO operation failed
        if (!bResult) {
            printf("[!] GetQueueCompletionStatus [%d] Failed With Error: %d\n", __LINE__, GetLastError());

            if (pIoData->pChunkBuffer)
                LocalFree(pIoData->pChunkBuffer);
            LocalFree(pIoData);
            //Decrement the global operation count.
            if (InterlockedDecrement64(&g_OpCount) == 0x00)
                SetEvent(g_hCompletionEvent); // signal that all operations are complete if the counter arrive at zero.
            continue;
        }
        // Process the completion based on the type of operation
        if (pIoData->IoOperation == IO_READ) {
            if (dwBytesTransferred > 0) {
                pIoData->dwBytesTransferred = dwBytesTransferred;
                pIoData->IoOperation = IO_WRITE;

                if (!WriteFile(pIoData->hDestFile, pIoData->pChunkBuffer, dwBytesTransferred, NULL, (LPOVERLAPPED)pIoData)) {
                    if ((dwError = GetLastError()) != ERROR_IO_PENDING) {
                        printf("[!] WriteFile [%d] Failed With Error: %d \n", __LINE__, dwError);

                        LocalFree(pIoData->pChunkBuffer);
                        LocalFree(pIoData);

                        if (InterlockedDecrement64(&g_OpCount) == 0x00)
                            SetEvent(g_hCompletionEvent); // signal that all operations are complete if the counter arrive at zero.
                    
                        continue;
                    }
                }
                //schedule next read if more data remains
                ulOffset.LowPart = pIoData->Overlapped.Offset;
                ulOffset.HighPart = pIoData->Overlapped.OffsetHigh;
                ulOffset.QuadPart += CHUNK_SIZE;

                if (ulOffset.QuadPart < pIoData->ullFileSize) {
                    if (!(pNewIoData = (PIO_DATA)LocalAlloc(LPTR, sizeof(IO_DATA)))) {
                        printf("[!] LocalAlloc [%d] Failed With Error: %d \n", __LINE__, dwError);
                    }
                    else {
                        if (!(pNewIoData->pChunkBuffer = (PBYTE)LocalAlloc(LPTR, CHUNK_SIZE))) {
                            printf("[!] LocalAlloc [%d] Failed With Error: %d \n", __LINE__, dwError);
                            LocalFree(pNewIoData);
                        }
                        else {
                            //Copy necessary parameters to the new IO_DATA struct
                            pNewIoData->ullFileSize = pIoData->ullFileSize;
                            pNewIoData->hSourceFile = pIoData->hSourceFile;
                            pNewIoData->hDestFile = pIoData->hDestFile;
                            pNewIoData->Overlapped.Offset = ulOffset.LowPart;
                            pNewIoData->Overlapped.OffsetHigh = ulOffset.HighPart;
                            pNewIoData->Overlapped.hEvent = NULL;
                            pNewIoData->IoOperation = IO_READ;

                            InterlockedIncrement64(&g_OpCount);

                            if (!ReadFile(pNewIoData->hSourceFile, pNewIoData->pChunkBuffer, CHUNK_SIZE, NULL, (LPOVERLAPPED)pNewIoData)) {
                                if ((dwError = GetLastError()) != ERROR_IO_PENDING) {
                                    printf("[!] ReadFile [%d] Failed With Error: %d \n", __LINE__, dwError);

                                    LocalFree(pNewIoData->pChunkBuffer);
                                    LocalFree(pNewIoData);

                                    if (InterlockedDecrement64(&g_OpCount) == 0x00)
                                        SetEvent(g_hCompletionEvent); // signal that all operations are complete if the counter arrive at zero.

                                    continue;
                                }
                            }
                        }
                    }
                }
            }
            else {
                //EOF reached
                LocalFree(pIoData->pChunkBuffer);
                LocalFree(pIoData);
                if (InterlockedDecrement64(&g_OpCount) == 0x00)
                    SetEvent(g_hCompletionEvent); // signal that all operations are complete if the counter arrive at zero.
            }
        }
        else if (pIoData->IoOperation == IO_WRITE) {
            LocalFree(pIoData->pChunkBuffer);
            LocalFree(pIoData);
            if (InterlockedDecrement64(&g_OpCount) == 0x00)
                SetEvent(g_hCompletionEvent); // signal that all operations are complete if the counter arrive at zero.

        }
    }
    return 0;
}
// StartCopy perfomrs the asynch copy using IOCP
BOOL StartCopy(IN LPCSTR lpSourceFilePath, IN LPCSTR lpDestDir) {
    HANDLE hSourceFile = INVALID_HANDLE_VALUE,
        hDestFile = INVALID_HANDLE_VALUE;
    PIO_DATA pIoData = NULL;
    LARGE_INTEGER liFileSize = { 0 };
    CHAR cDestPath[MAX_PATH] = { 0 };
    DWORD dwError = 0x00;
    BOOL bResult = FALSE;
    //open file for asynch reading
    if ((hSourceFile = CreateFileA(lpSourceFilePath, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL)) == INVALID_HANDLE_VALUE) {
        printf("[!] CreateFileA [%ld] Failed With Error: %lu\n", __LINE__, GetLastError());
        goto _END_OF_FUNC;
    }

    if (!GetFileSizeEx(hSourceFile, &liFileSize)) {
        printf("[!] GetFileSizeEx [%ld] Failed With Error: %lu\n", __LINE__, GetLastError());
        goto _END_OF_FUNC;
    }

    snprintf(cDestPath, MAX_PATH, "%s\\%s", lpDestDir, GET_FILENAME(lpSourceFilePath));

    if ((hDestFile = CreateFileA(cDestPath, GENERIC_WRITE, 0x00, NULL, CREATE_ALWAYS, FILE_FLAG_OVERLAPPED, NULL)) == INVALID_HANDLE_VALUE) {
        printf("[!] CreateFileA [%ld] Failed With Error: %lu\n", __LINE__, GetLastError());
        goto _END_OF_FUNC;
    }

    //Preallocate Dest file
    if(!SetFilePointerEx(hDestFile, liFileSize, NULL, FILE_BEGIN)) {
        printf("[!] SetFilePointerEx [%ld] Failed With Error: %lu\n", __LINE__, GetLastError());
        goto _END_OF_FUNC;
    }
    if(!SetEndOfFile(hDestFile)) {
        printf("[!] SetEndOfFile [%ld] Failed With Error: %lu\n", __LINE__, GetLastError());
        goto _END_OF_FUNC;
    }
    // Create I/O completion port
    if (!(g_hICOP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 16))) {
        printf("[!] CreateIoCompletionPort [%ld] Failed With Error: %lu\n", __LINE__, GetLastError());
        goto _END_OF_FUNC;
    }

    //associate both files with completion port
    if (!CreateIoCompletionPort(hSourceFile, g_hICOP, (ULONG_PTR)hSourceFile, 0)) {
        printf("[!] CreateIoCompletionPort for Sourcefile [%ld] Failed With Error: %lu\n", __LINE__, GetLastError());
        goto _END_OF_FUNC;
    }
    if (!CreateIoCompletionPort(hDestFile, g_hICOP, (ULONG_PTR)hSourceFile, 0)) {
        printf("[!] CreateIoCompletionPort for Destfile [%ld] Failed With Error: %lu\n", __LINE__, GetLastError());
        goto _END_OF_FUNC;
    }

    //Create worker Thread to process I/O completions passing IOCP handle as param
    if (!(g_hworkerThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)WorkerThreadProc, g_hICOP, 0, NULL))) {
        printf("[!] CreateThread [%ld] Failed With Error: %lu\n", __LINE__, GetLastError());
        goto _END_OF_FUNC;
    }

    //init op count
    InterlockedExchange64(&g_OpCount, 1);

    if (!(pIoData = (PIO_DATA)LocalAlloc(LPTR, sizeof(IO_DATA)))) {
        printf("[!] LocalAlloc [%ld] Failed With Error: %lu\n", __LINE__, GetLastError());
        goto _END_OF_FUNC;
    }

    if (!(pIoData->pChunkBuffer = (PBYTE)LocalAlloc(LPTR, CHUNK_SIZE))) {
        printf("[!] LocalAlloc [%ld] Failed With Error: %lu\n", __LINE__, GetLastError());
        LocalFree(pIoData);
        goto _END_OF_FUNC;
    }
    pIoData->ullFileSize = liFileSize.QuadPart;
        pIoData->hSourceFile = hSourceFile;
    pIoData->hDestFile = hDestFile;
    pIoData->Overlapped.Offset = 0x00;
    pIoData->Overlapped.OffsetHigh = 0x00;
    pIoData->Overlapped.hEvent = NULL;
    pIoData->IoOperation = IO_READ;

    // Start the first asynchronous read.
    if (!ReadFile(hSourceFile, pIoData->pChunkBuffer, CHUNK_SIZE, NULL, (LPOVERLAPPED)pIoData)) {
        if ((dwError = GetLastError()) != ERROR_IO_PENDING) {
            printf("[!] WriteFile [%ld] Failed With Error: %ld \n", __LINE__, dwError);
            LocalFree(pIoData->pChunkBuffer);
            LocalFree(pIoData);
            goto _END_OF_FUNC;
        }
    }
    // Create an event to signal completion.
    if (!(g_hCompletionEvent = CreateEvent(NULL, TRUE, FALSE, NULL))) {
        printf("[!] CreateEvent failed with error: %ld\n", GetLastError());
        goto _END_OF_FUNC;
    }
    /*
          At this point, the asynchronous operations are in progress.
          Additional logic can be executed here while the I/O operations are processed.
      */

      // Wait until all I/O operations are complete.
      // g_hCompleteEvent is signaled when g_OpCount reaches zero.
    WaitForSingleObject(g_hCompletionEvent, INFINITE);

    // When the event is signaled, we send a termination signal to the worker thread.
    PostQueuedCompletionStatus(g_hICOP, 0, 0, NULL);

    // Wait for the worker thread to terminate.
    WaitForSingleObject(g_hworkerThread, INFINITE);

#ifdef DEBUG
    printf("[*] Copy Completed Successfully.\n");
    printf("[i] Copied:\n\t[>] From: %s\n\t[>] To: %s\n\t[>] Copied Bytes: %llu\n", lpSourceFilePath, cDestPath, liFileSize.QuadPart);
#endif // DEBUG


    bResult = TRUE;

_END_OF_FUNC:
    if (g_hICOP)
        CloseHandle(g_hICOP);
    if (g_hworkerThread)
        CloseHandle(g_hworkerThread);
    if (hSourceFile != INVALID_HANDLE_VALUE)
        CloseHandle(hSourceFile);
    if (hDestFile != INVALID_HANDLE_VALUE)
        CloseHandle(hDestFile);
    if (g_hCompletionEvent)
        CloseHandle(g_hCompletionEvent);

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

    // start multiple copy operations
    for (int i = 0; i < 0x10; i++)
    {
        if (!StartCopy(argv[1], argv[2])) {
            printf("[!] Copy Operation Failed\n");
            return -1;
        }
    }

    QueryPerformanceCounter(&ExecEnd);
    ExecTime = (DOUBLE)(ExecEnd.QuadPart - ExecStart.QuadPart) / Frequency.QuadPart;
    printf("[*] Execution Time: %f Seconds\n", ExecTime);
    return 0;
}