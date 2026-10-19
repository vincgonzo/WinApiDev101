
#include <windows.h>
#include <stdio.h>
#include <strsafe.h>

#define PRINT_DIR_NAME
#define PRINT_FILE_NAME
#define MAX_PATH_BUFFER 32768

volatile LONG64 g_FileCntr = 0;

typedef struct _THREAD_PARMS {
    LPWSTR szDirectory;
    DWORD dwMaxDepth;
} THREAD_PARMS, * PTHREAD_PARMS;

BOOL FilesEnumStartForDirectory(LPCWSTR szDirectory, DWORD dwCurrentDepth, DWORD dwMaxDepth);
DWORD WINAPI FileEnumThreadFunc(LPVOID lpParameter);

VOID PrintUsage(LPCWSTR szProgramName) {
    wprintf(L"\nFileEnumAlgorithm - Recursive file enumeration\n"
        L"===============================================\n\n"
        L"Usage:\n  %s <directory> <max-depth>\n\n"
        L"Arguments:\n  <directory>   Directory to enumerate.\n"
        L"  <max-depth>   Maximum recursion depth.\n\n"
        L"Depth examples:\n  0 = Target directory only\n"
        L"  1 = Target + immediate subdirectories\n"
        L"  2 = Target + 2 levels of subdirectories\n"
        L"  3 = Target + 3 levels of subdirectories\n\n"
        L"Examples:\n  %s \"C:\\Users\\Alice\\Desktop\" 0\n"
        L"  %s \"C:\\Users\\Alice\\Desktop\" 1\n"
        L"  %s \"C:\\Users\\Alice\\Desktop\" 2\n"
        L"  %s \"C:\\Temp\" 3\n\n",
        szProgramName, szProgramName, szProgramName, szProgramName, szProgramName);
}

BOOL FilesEnumStartForDirectory(LPCWSTR szDirectory, DWORD dwCurrentDepth, DWORD dwMaxDepth) {
    if (!szDirectory || !szDirectory[0]) return FALSE;

    HANDLE hFind = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW FindFileData = { 0 };
    WCHAR szSearchPath[MAX_PATH_BUFFER] = { 0 }, szFullPath[MAX_PATH_BUFFER] = { 0 };
    SIZE_T dirLen = wcslen(szDirectory);

    HRESULT hr = StringCchPrintfW(szSearchPath, ARRAYSIZE(szSearchPath),
        szDirectory[dirLen - 1] == L'\\' ? L"%s*.*" : L"%s\\*.*",
        szDirectory);

    if (FAILED(hr)) {
        wprintf(L"[!] Search path is too long: %s\n", szDirectory);
        return FALSE;
    }

    hFind = FindFirstFileW(szSearchPath, &FindFileData);

    if (hFind == INVALID_HANDLE_VALUE) {
        DWORD dwError = GetLastError();

        if (dwError == ERROR_ACCESS_DENIED)
            wprintf(L"[!] Access denied: %s\n", szDirectory);
        else
            wprintf(L"[!] FindFirstFileW failed for [%s] - Error: %lu\n", szDirectory, dwError);

        return FALSE;
    }

    do {
        if (!lstrcmpW(FindFileData.cFileName, L".") ||
            !lstrcmpW(FindFileData.cFileName, L".."))
            continue;

        hr = StringCchPrintfW(szFullPath, ARRAYSIZE(szFullPath),
            szDirectory[dirLen - 1] == L'\\' ? L"%s%s" : L"%s\\%s",
            szDirectory, FindFileData.cFileName);

        if (FAILED(hr)) {
            wprintf(L"[!] Path too long: %s\\%s\n", szDirectory, FindFileData.cFileName);
            continue;
        }

        if (FindFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
#ifdef PRINT_DIR_NAME
            wprintf(L"[DIRECTORY] [Depth %lu] %s\n", dwCurrentDepth + 1, szFullPath);
#endif

            if (FindFileData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                continue;

            if (dwCurrentDepth < dwMaxDepth)
                FilesEnumStartForDirectory(szFullPath, dwCurrentDepth + 1, dwMaxDepth);
        }
        else {
#ifdef PRINT_FILE_NAME
            wprintf(L"[FILE] [Depth %lu] %s\n", dwCurrentDepth, szFullPath);
#endif
            InterlockedIncrement64(&g_FileCntr);
        }
    } while (FindNextFileW(hFind, &FindFileData));

    DWORD dwError = GetLastError();

    if (dwError != ERROR_NO_MORE_FILES)
        wprintf(L"[!] FindNextFileW failed in [%s] - Error: %lu\n", szDirectory, dwError);

    FindClose(hFind);
    return TRUE;
}

DWORD WINAPI FileEnumThreadFunc(LPVOID lpParameter) {
    PTHREAD_PARMS pThreadParms = (PTHREAD_PARMS)lpParameter;

    if (!pThreadParms) return 1;

    FilesEnumStartForDirectory(pThreadParms->szDirectory, 0, pThreadParms->dwMaxDepth);

    if (pThreadParms->szDirectory)
        LocalFree(pThreadParms->szDirectory);

    LocalFree(pThreadParms);
    return 0;
}

BOOL ParseDepth(LPCWSTR szDepth, DWORD* pdwDepth) {
    if (!szDepth || !pdwDepth || !szDepth[0] || szDepth[0] == L'-')
        return FALSE;

    WCHAR* pEnd = NULL;
    unsigned long ulDepth = wcstoul(szDepth, &pEnd, 10);

    if (pEnd == szDepth || *pEnd != L'\0' || ulDepth > MAXDWORD)
        return FALSE;

    *pdwDepth = (DWORD)ulDepth;
    return TRUE;
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 3) {
        wprintf(L"\n[!] Invalid number of arguments.\n");
        PrintUsage(argv[0]);
        return 1;
    }

    LPCWSTR szTargetDirectory = argv[1];
    DWORD dwMaxDepth = 0;

    if (!szTargetDirectory[0]) {
        wprintf(L"[!] Target directory cannot be empty.\n");
        PrintUsage(argv[0]);
        return 1;
    }

    if (!ParseDepth(argv[2], &dwMaxDepth)) {
        wprintf(L"[!] Invalid maximum depth: %s\n", argv[2]);
        PrintUsage(argv[0]);
        return 1;
    }

    DWORD dwAttributes = GetFileAttributesW(szTargetDirectory);

    if (dwAttributes == INVALID_FILE_ATTRIBUTES) {
        wprintf(L"[!] Target does not exist or cannot be accessed:\n    %s\n    Error: %lu\n",
            szTargetDirectory, GetLastError());
        return 1;
    }

    if (!(dwAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        wprintf(L"[!] Target is not a directory:\n    %s\n", szTargetDirectory);
        return 1;
    }

    wprintf(L"\n===============================================\n"
        L" File Enumeration\n"
        L"===============================================\n"
        L" Target      : %s\n"
        L" Max depth   : %lu\n"
        L"===============================================\n\n",
        szTargetDirectory, dwMaxDepth);

    PTHREAD_PARMS pThreadParms = (PTHREAD_PARMS)LocalAlloc(LPTR, sizeof(THREAD_PARMS));

    if (!pThreadParms) {
        wprintf(L"[!] LocalAlloc failed - Error: %lu\n", GetLastError());
        return 1;
    }

    SIZE_T ccLength = wcslen(szTargetDirectory) + 1;
    pThreadParms->szDirectory = (LPWSTR)LocalAlloc(LPTR, ccLength * sizeof(WCHAR));

    if (!pThreadParms->szDirectory) {
        wprintf(L"[!] LocalAlloc for target directory failed - Error: %lu\n", GetLastError());
        LocalFree(pThreadParms);
        return 1;
    }

    if (FAILED(StringCchCopyW(pThreadParms->szDirectory, ccLength, szTargetDirectory))) {
        wprintf(L"[!] Failed to copy target directory.\n");
        LocalFree(pThreadParms->szDirectory);
        LocalFree(pThreadParms);
        return 1;
    }

    pThreadParms->dwMaxDepth = dwMaxDepth;

    DWORD dwThreadId = 0;
    HANDLE hThread = CreateThread(NULL, 0, FileEnumThreadFunc, pThreadParms, 0, &dwThreadId);

    if (!hThread) {
        wprintf(L"[!] CreateThread failed - Error: %lu\n", GetLastError());
        LocalFree(pThreadParms->szDirectory);
        LocalFree(pThreadParms);
        return 1;
    }

    wprintf(L"[+] Created enumeration thread [ %lu ]\n\n", dwThreadId);

    DWORD dwWaitResult = WaitForSingleObject(hThread, INFINITE);

    if (dwWaitResult == WAIT_FAILED)
        wprintf(L"[!] WaitForSingleObject failed - Error: %lu\n", GetLastError());

    CloseHandle(hThread);

    wprintf(L"\n===============================================\n"
        L" Enumeration Complete\n"
        L"===============================================\n"
        L" Targeted files : %lld\n"
        L"===============================================\n\n",
        g_FileCntr);

    return 0;
}