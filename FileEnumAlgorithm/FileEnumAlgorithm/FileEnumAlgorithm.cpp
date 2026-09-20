#include <windows.h>
#include <stdio.h>
#include <strsafe.h>

#define MAX_DRIVES_NBR 26

volatile LONG64 g_FileCntr = 0x00;

typedef struct _THREAD_PARMS {
    LPWSTR szDirectory; // Directory path to enumerate
} THREAD_PARMS, *PTHREAD_PARMS;


BOOL FilesEnumStartForDrive(IN LPWSTR szDirectory) {
    HANDLE hFind = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW FindFileData = { 0 };
    WCHAR szDirBuff1[MAX_PATH] = { 0 },
        szDirBuff2[MAX_PATH] = { 0 };
    BOOL bResult = FALSE;

    if (!szDirectory) return FALSE;
    //Build search path
    if (szDirectory[lstrlen(szDirectory) - 1] != L'\\') {
        if (FAILED(StringCchPrintfW(szDirBuff1, MAX_PATH, L"% s\\ * .*", szDirectory))) {
            goto _END_OF_FUNC;
        }
    }
    else {
        if (FAILED(StringCchPrintfW(szDirBuff1, MAX_PATH, L"% s * .*", szDirectory))) {
            goto _END_OF_FUNC;
        }
    }

    if ((hFind = FindFirstFileW(szDirBuff1, &FindFileData)) == INVALID_HANDLE_VALUE)
        return FALSE;

    do 
    {
        SIZE_T ccLength = lstrlenW(szDirectory); //current directory length
        if (lstrcmpW(FindFileData.cFileName, L".") == 0 || lstrcmpW(FindFileData.cFileName, L"..") == 0)
            continue;

        //build full path in DirBuff2
        if (ccLength > 0x00 && szDirectory[ccLength - 0x01] == L'\\') {
            if (FAILED(StringCchPrintfW(szDirBuff1, MAX_PATH, L"%s%s", szDirectory, FindFileData.cFileName))) {
                goto _END_OF_FUNC;
            }
        }
        else {
            if (FAILED(StringCchPrintfW(szDirBuff1, MAX_PATH, L"%s\\%s", szDirectory, FindFileData.cFileName))) {
                goto _END_OF_FUNC;
            }
        }

        //check if directory
        if (FindFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
#ifdef PRINT_DIR_NAME
            wprintf(L"[DIRECTORY] %s\n", szDirBuff2);
#endif // PRINT_DIR_NAME
            FilesEnumStartForDrive(szDirBuff1);
        }// it's a file
        else {
#ifdef PRINT_DIR_NAME
            wprintf(L"[FILE] %s\n", szDirBuff2);
#endif // PRINT_DIR_NAME
            InterlockedIncrement64(&g_FileCntr);
        }
        //continue searching
    } while (FindNextFileW(hFind, &FindFileData));

    bResult = TRUE;
_END_OF_FUNC:
    if (hFind != INVALID_HANDLE_VALUE)
        FindClose(hFind);
    return bResult;
}

VOID FileEnumThreadFunc(IN LPVOID lpParameter) {
    PTHREAD_PARMS pThreadParms = (PTHREAD_PARMS)lpParameter;

    FilesEnumStartForDrive(pThreadParms->szDirectory);

    LocalFree(pThreadParms->szDirectory);
    LocalFree(pThreadParms);
}

BOOL DrivesEnumStart() {
    DWORD dwDriveBitMask = 0x00,
        dwCurrentThreadCntr = 0x00;
    HANDLE hThreadArray[MAX_DRIVES_NBR] = { 0 };

    if (!(dwDriveBitMask = GetLogicalDrives()))
        return FALSE;

    for (int i = 0; i < MAX_DRIVES_NBR; i++) {
        //check id drives exists
        if (dwDriveBitMask & (1 << i)) {
            DWORD dwDriveType = 0x00;
            DWORD dwThreadId = 0x00;
            WCHAR szDrivePath[4] = { 0 };
            SIZE_T ccLength = 0x00;
            PTHREAD_PARMS pThreadParms = NULL;

            wsprintf(szDrivePath, L"%C:\\", L'A' + i);
            dwDriveType = GetDriveTypeW(szDrivePath);

            if (dwDriveType != DRIVE_FIXED && dwDriveType != DRIVE_REMOVABLE)
                continue;

            if (!(pThreadParms = (PTHREAD_PARMS)LocalAlloc(LPTR, sizeof(THREAD_PARMS)))) {
                printf("[!] LocalAlloc %d Failed With Error: %d\n", __LINE__, GetLastError());
                return FALSE;
            }
            //Allocate mem for dir path
            ccLength = wcslen(szDrivePath) + 0x01;
            if (!(pThreadParms->szDirectory = (WCHAR*)LocalAlloc(LPTR, ccLength * sizeof(WCHAR)))) {
                printf("[!] LocalAlloc for Directory %d Failed With Error: %d\n", __LINE__, GetLastError());
                LocalFree(pThreadParms);
                return FALSE;
            }
            if (FAILED(StringCchPrintfW(pThreadParms->szDirectory, ccLength, L"%s", szDrivePath))) {
                LocalFree(pThreadParms->szDirectory);
                LocalFree(pThreadParms);
                return FALSE;
            }

            if (!(hThreadArray[dwCurrentThreadCntr] = CreateThread(NULL, 0x00, (LPTHREAD_START_ROUTINE)FileEnumThreadFunc, pThreadParms, 0x00, &dwThreadId))) {
                printf("[!] Failed To create new Thread For '%w' With Error: %d\n", szDrivePath, GetLastError());
                LocalFree(pThreadParms->szDirectory);
                LocalFree(pThreadParms);
                return FALSE;
            }
            printf("[!] Create new Thread [ %d ] Targeting [ %ws ] \n", dwThreadId, szDrivePath);

            dwCurrentThreadCntr++;
        }
    }


    WaitForMultipleObjects(dwCurrentThreadCntr, hThreadArray, TRUE, INFINITE);

    for (int i = 0; i < dwCurrentThreadCntr; i++) {
        CloseHandle(hThreadArray[i]);
    }

    return TRUE;
}



int main()
{
    printf("[#] Press <Enter> To Start ...");
    getchar();


    DrivesEnumStart();

    wprintf(L"[i] Total Targeted Files : %lld\n", g_FileCntr);

    printf("[#] Press <Enter> To Quit ...");
    getchar();

    return 0;
}