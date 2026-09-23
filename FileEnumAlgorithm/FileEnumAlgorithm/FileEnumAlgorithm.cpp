#include <windows.h>
#include <stdio.h>
#include <strsafe.h>

#define PRINT_DIR_NAME
#define PRINT_FILE_NAME

/*
 * Maximum number of simultaneous drive threads.
 * This is no longer really important because this version
 * targets one directory at a time.
 */
#define MAX_PATH_BUFFER 32768

    volatile LONG64 g_FileCntr = 0;


/*
 * Thread parameters
 */
typedef struct _THREAD_PARMS
{
    LPWSTR szDirectory;
    DWORD dwMaxDepth;

} THREAD_PARMS, * PTHREAD_PARMS;


/*
 * Function declarations
 */
BOOL FilesEnumStartForDirectory(
    IN LPCWSTR szDirectory,
    IN DWORD dwCurrentDepth,
    IN DWORD dwMaxDepth
);

DWORD WINAPI FileEnumThreadFunc(
    IN LPVOID lpParameter
);


/*
 * Display program usage.
 */
VOID PrintUsage(
    IN LPCWSTR szProgramName
)
{
    wprintf(
        L"\n"
        L"FileEnumAlgorithm - Recursive file enumeration\n"
        L"===============================================\n"
        L"\n"
        L"Usage:\n"
        L"  %s <directory> <max-depth>\n"
        L"\n"
        L"Arguments:\n"
        L"  <directory>   Directory to enumerate.\n"
        L"  <max-depth>   Maximum recursion depth.\n"
        L"\n"
        L"Depth examples:\n"
        L"  0  = Target directory only\n"
        L"  1  = Target + immediate subdirectories\n"
        L"  2  = Target + 2 levels of subdirectories\n"
        L"  3  = Target + 3 levels of subdirectories\n"
        L"\n"
        L"Examples:\n"
        L"  %s \"C:\\Users\\Alice\\Desktop\" 0\n"
        L"  %s \"C:\\Users\\Alice\\Desktop\" 1\n"
        L"  %s \"C:\\Users\\Alice\\Desktop\" 2\n"
        L"  %s \"C:\\Temp\" 3\n"
        L"\n",
        szProgramName,
        szProgramName,
        szProgramName,
        szProgramName,
        szProgramName
    );
}


/*
 * Recursively enumerate a directory.
 *
 * dwCurrentDepth:
 *      Current recursion depth.
 *
 * dwMaxDepth:
 *      Maximum depth we are allowed to enter.
 */
BOOL FilesEnumStartForDirectory(
    IN LPCWSTR szDirectory,
    IN DWORD dwCurrentDepth,
    IN DWORD dwMaxDepth
)
{
    HANDLE hFind = INVALID_HANDLE_VALUE;

    WIN32_FIND_DATAW FindFileData = { 0 };

    WCHAR szSearchPath[MAX_PATH_BUFFER] = { 0 };
    WCHAR szFullPath[MAX_PATH_BUFFER] = { 0 };

    HRESULT hr;

    if (szDirectory == NULL || szDirectory[0] == L'\0')
        return FALSE;


    /*
     * Build search pattern.
     *
     * C:\Test
     *      ->
     * C:\Test\*.*
     */
    if (szDirectory[lstrlenW(szDirectory) - 1] == L'\\')
    {
        hr = StringCchPrintfW(
            szSearchPath,
            ARRAYSIZE(szSearchPath),
            L"%s*.*",
            szDirectory
        );
    }
    else
    {
        hr = StringCchPrintfW(
            szSearchPath,
            ARRAYSIZE(szSearchPath),
            L"%s\\*.*",
            szDirectory
        );
    }


    if (FAILED(hr))
    {
        wprintf(
            L"[!] Search path is too long: %s\n",
            szDirectory
        );

        return FALSE;
    }


    /*
     * Start enumeration.
     */
    hFind = FindFirstFileW(
        szSearchPath,
        &FindFileData
    );


    if (hFind == INVALID_HANDLE_VALUE)
    {
        DWORD dwError = GetLastError();

        /*
         * Access denied is common when enumerating some
         * Windows directories.
         */
        if (dwError == ERROR_ACCESS_DENIED)
        {
            wprintf(
                L"[!] Access denied: %s\n",
                szDirectory
            );
        }
        else
        {
            wprintf(
                L"[!] FindFirstFileW failed for [%s] "
                L"- Error: %lu\n",
                szDirectory,
                dwError
            );
        }

        return FALSE;
    }


    /*
     * Enumerate directory contents.
     */
    do
    {
        /*
         * Ignore "." and "..".
         */
        if (lstrcmpW(
            FindFileData.cFileName,
            L".") == 0 ||
            lstrcmpW(
                FindFileData.cFileName,
                L"..") == 0)
        {
            continue;
        }


        /*
         * Build complete path.
         */
        if (szDirectory[lstrlenW(szDirectory) - 1] == L'\\')
        {
            hr = StringCchPrintfW(
                szFullPath,
                ARRAYSIZE(szFullPath),
                L"%s%s",
                szDirectory,
                FindFileData.cFileName
            );
        }
        else
        {
            hr = StringCchPrintfW(
                szFullPath,
                ARRAYSIZE(szFullPath),
                L"%s\\%s",
                szDirectory,
                FindFileData.cFileName
            );
        }


        if (FAILED(hr))
        {
            wprintf(
                L"[!] Path too long: %s\\%s\n",
                szDirectory,
                FindFileData.cFileName
            );

            continue;
        }


        /*
         * Is this a directory?
         */
        if (FindFileData.dwFileAttributes &
            FILE_ATTRIBUTE_DIRECTORY)
        {
#ifdef PRINT_DIR_NAME

            wprintf(
                L"[DIRECTORY] [Depth %lu] %s\n",
                dwCurrentDepth + 1,
                szFullPath
            );

#endif /* PRINT_DIR_NAME */


            /*
             * Do not follow reparse points.
             *
             * This prevents following junctions/symbolic links
             * into unrelated directory trees or creating loops.
             */
            if (FindFileData.dwFileAttributes &
                FILE_ATTRIBUTE_REPARSE_POINT)
            {
                continue;
            }


            /*
             * Only recurse when we haven't reached
             * the maximum depth.
             */
            if (dwCurrentDepth < dwMaxDepth)
            {
                FilesEnumStartForDirectory(
                    szFullPath,
                    dwCurrentDepth + 1,
                    dwMaxDepth
                );
            }
        }
        else
        {
#ifdef PRINT_FILE_NAME

            wprintf(
                L"[FILE] [Depth %lu] %s\n",
                dwCurrentDepth,
                szFullPath
            );

#endif /* PRINT_FILE_NAME */


            /*
             * Thread-safe global counter.
             */
            InterlockedIncrement64(
                &g_FileCntr
            );
        }

    } while (FindNextFileW(
        hFind,
        &FindFileData
    ));


    /*
     * FindNextFileW normally ends with
     * ERROR_NO_MORE_FILES.
     */
    {
        DWORD dwError = GetLastError();

        if (dwError != ERROR_NO_MORE_FILES)
        {
            wprintf(
                L"[!] FindNextFileW failed in [%s] "
                L"- Error: %lu\n",
                szDirectory,
                dwError
            );
        }
    }


    /*
     * Close enumeration handle.
     */
    FindClose(hFind);

    return TRUE;
}


/*
 * Thread entry point.
 */
DWORD WINAPI FileEnumThreadFunc(
    IN LPVOID lpParameter
)
{
    PTHREAD_PARMS pThreadParms =
        (PTHREAD_PARMS)lpParameter;


    if (pThreadParms == NULL)
        return 1;


    /*
     * Enumerate target directory.
     */
    FilesEnumStartForDirectory(
        pThreadParms->szDirectory,
        0,
        pThreadParms->dwMaxDepth
    );


    /*
     * Free allocated memory.
     */
    if (pThreadParms->szDirectory != NULL)
    {
        LocalFree(
            pThreadParms->szDirectory
        );
    }


    LocalFree(
        pThreadParms
    );


    return 0;
}


/*
 * Convert a command-line string to DWORD.
 */
BOOL ParseDepth(
    IN LPCWSTR szDepth,
    OUT DWORD* pdwDepth
)
{
    WCHAR* pEnd = NULL;

    unsigned long ulDepth;


    if (szDepth == NULL ||
        pdwDepth == NULL ||
        szDepth[0] == L'\0')
    {
        return FALSE;
    }


    /*
     * Reject negative values.
     */
    if (szDepth[0] == L'-')
        return FALSE;


    ulDepth = wcstoul(
        szDepth,
        &pEnd,
        10
    );


    /*
     * Make sure the entire argument was numeric.
     */
    if (pEnd == szDepth ||
        *pEnd != L'\0')
    {
        return FALSE;
    }


    /*
     * Make sure conversion didn't overflow DWORD.
     */
    if (ulDepth > MAXDWORD)
        return FALSE;


    *pdwDepth = (DWORD)ulDepth;

    return TRUE;
}


/*
 * Main.
 */
int wmain(
    int argc,
    wchar_t* argv[]
)
{
    LPCWSTR szTargetDirectory;

    DWORD dwMaxDepth = 0;

    HANDLE hThread = NULL;

    PTHREAD_PARMS pThreadParms = NULL;

    DWORD dwThreadId = 0;

    DWORD dwWaitResult;


    /*
     * Check command-line arguments.
     */
    if (argc != 3)
    {
        wprintf(
            L"\n[!] Invalid number of arguments.\n"
        );

        PrintUsage(
            argv[0]
        );

        return 1;
    }


    /*
     * First argument:
     *
     * Target directory.
     */
    szTargetDirectory = argv[1];


    /*
     * Make sure directory isn't empty.
     */
    if (szTargetDirectory[0] == L'\0')
    {
        wprintf(
            L"[!] Target directory cannot be empty.\n"
        );

        PrintUsage(
            argv[0]
        );

        return 1;
    }


    /*
     * Second argument:
     *
     * Maximum depth.
     */
    if (!ParseDepth(
        argv[2],
        &dwMaxDepth))
    {
        wprintf(
            L"[!] Invalid maximum depth: %s\n",
            argv[2]
        );

        PrintUsage(
            argv[0]
        );

        return 1;
    }


    /*
     * Verify that the target exists and is a directory.
     */
    {
        DWORD dwAttributes;

        dwAttributes =
            GetFileAttributesW(
                szTargetDirectory
            );


        if (dwAttributes == INVALID_FILE_ATTRIBUTES)
        {
            wprintf(
                L"[!] Target does not exist or cannot be accessed:\n"
                L"    %s\n"
                L"    Error: %lu\n",
                szTargetDirectory,
                GetLastError()
            );

            return 1;
        }


        if (!(dwAttributes &
            FILE_ATTRIBUTE_DIRECTORY))
        {
            wprintf(
                L"[!] Target is not a directory:\n"
                L"    %s\n",
                szTargetDirectory
            );

            return 1;
        }
    }


    /*
     * Display configuration.
     */
    wprintf(
        L"\n"
        L"===============================================\n"
        L" File Enumeration\n"
        L"===============================================\n"
        L" Target      : %s\n"
        L" Max depth   : %lu\n"
        L"===============================================\n"
        L"\n",
        szTargetDirectory,
        dwMaxDepth
    );


    /*
     * Allocate thread parameters.
     */
    pThreadParms =
        (PTHREAD_PARMS)LocalAlloc(
            LPTR,
            sizeof(THREAD_PARMS)
        );


    if (pThreadParms == NULL)
    {
        wprintf(
            L"[!] LocalAlloc failed - Error: %lu\n",
            GetLastError()
        );

        return 1;
    }


    /*
     * Allocate memory for target directory.
     */
    {
        SIZE_T ccLength;

        ccLength =
            wcslen(szTargetDirectory) + 1;


        pThreadParms->szDirectory =
            (LPWSTR)LocalAlloc(
                LPTR,
                ccLength * sizeof(WCHAR)
            );


        if (pThreadParms->szDirectory == NULL)
        {
            wprintf(
                L"[!] LocalAlloc for target directory failed "
                L"- Error: %lu\n",
                GetLastError()
            );

            LocalFree(
                pThreadParms
            );

            return 1;
        }


        if (FAILED(StringCchCopyW(
            pThreadParms->szDirectory,
            ccLength,
            szTargetDirectory
        )))
        {
            wprintf(
                L"[!] Failed to copy target directory.\n"
            );

            LocalFree(
                pThreadParms->szDirectory
            );

            LocalFree(
                pThreadParms
            );

            return 1;
        }
    }


    /*
     * Store maximum depth.
     */
    pThreadParms->dwMaxDepth =
        dwMaxDepth;


    /*
     * Create enumeration thread.
     */
    hThread =
        CreateThread(
            NULL,
            0,
            FileEnumThreadFunc,
            pThreadParms,
            0,
            &dwThreadId
        );


    if (hThread == NULL)
    {
        wprintf(
            L"[!] CreateThread failed "
            L"- Error: %lu\n",
            GetLastError()
        );

        LocalFree(
            pThreadParms->szDirectory
        );

        LocalFree(
            pThreadParms
        );

        return 1;
    }


    wprintf(
        L"[+] Created enumeration thread [ %lu ]\n"
        L"\n",
        dwThreadId
    );


    /*
     * Wait for enumeration to finish.
     */
    dwWaitResult =
        WaitForSingleObject(
            hThread,
            INFINITE
        );


    if (dwWaitResult == WAIT_FAILED)
    {
        wprintf(
            L"[!] WaitForSingleObject failed "
            L"- Error: %lu\n",
            GetLastError()
        );
    }


    /*
     * Close thread handle.
     */
    CloseHandle(
        hThread
    );


    /*
     * Display result.
     */
    wprintf(
        L"\n"
        L"===============================================\n"
        L" Enumeration Complete\n"
        L"===============================================\n"
        L" Targeted files : %lld\n"
        L"===============================================\n"
        L"\n",
        g_FileCntr
    );


    return 0;
}