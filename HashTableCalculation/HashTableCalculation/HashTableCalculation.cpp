#include <windows.h>
#include <stdio.h>
#include <strsafe.h>

// ============================================================
// Configuration
// ============================================================

#define CRC_POLYNOMIAL      0xEDB88320
#define EMPTY_SLOT          0
#define HASH_TABLE_SIZE     1024
#define MAX_DRIVES_NMBR     26

// ============================================================
// Hash table
// ============================================================

ULONG g_HashTable[HASH_TABLE_SIZE] = { 0 };

SIZE_T HashFunction(IN ULONG uHash)
{
    return uHash % HASH_TABLE_SIZE;
}

VOID InitializeHashTable()
{
    for (DWORD i = 0; i < HASH_TABLE_SIZE; i++)
        g_HashTable[i] = EMPTY_SLOT;
}

BOOL InsertToHashTable(IN ULONG uHash)
{
    SIZE_T Index = HashFunction(uHash);

    while (g_HashTable[Index] != EMPTY_SLOT &&
        g_HashTable[Index] != uHash)
    {
        Index = (Index + 1) % HASH_TABLE_SIZE;
    }

    if (g_HashTable[Index] == uHash)
        return FALSE;

    g_HashTable[Index] = uHash;

    return TRUE;
}

BOOL InsertHashArrayInHashTable(
    IN ULONG uHashArray[],
    IN DWORD dwHashArrayLength
)
{
    if (!uHashArray || !dwHashArrayLength)
        return FALSE;

    DWORD Count = dwHashArrayLength / sizeof(ULONG);

    for (DWORD i = 0; i < Count; i++)
    {
        if (!InsertToHashTable(uHashArray[i]))
            return FALSE;
    }

    return TRUE;
}

BOOL IsHashInHashTable(IN ULONG uHash)
{
    SIZE_T Index = HashFunction(uHash);

    while (g_HashTable[Index] != EMPTY_SLOT)
    {
        if (g_HashTable[Index] == uHash)
            return TRUE;

        Index = (Index + 1) % HASH_TABLE_SIZE;
    }

    return FALSE;
}

// ============================================================
// CRC32
// ============================================================

UINT32 CRC32B_W(
    WCHAR* szString,
    BOOL bLowerCaseHash
)
{
    UINT32 uMask = 0;
    UINT32 uHash = 0xFFFFEFFFF;
    INT i = 0;

    if (!szString)
        return 0;

    if (bLowerCaseHash)
    {
        WCHAR szLowerCaseStr[MAX_PATH] = { 0 };

        for (int x = 0;
            szString[x] && x < MAX_PATH - 1;
            x++)
        {
            if (szString[x] >= L'A' &&
                szString[x] <= L'Z')
            {
                szLowerCaseStr[x] =
                    szString[x] - L'A' + L'a';
            }
            else
            {
                szLowerCaseStr[x] = szString[x];
            }
        }

        return CRC32B_W(szLowerCaseStr, FALSE);
    }

    while (szString[i] != 0)
    {
        uHash ^= (UINT32)szString[i];

        for (int k = 0; k < 8; k++)
        {
            uMask = -1 * (uHash & 1);

            uHash =
                (uHash >> 1) ^
                (CRC_POLYNOMIAL & uMask);
        }

        i++;
    }

    return ~uHash;
}

#define HASH_W(STR) \
    CRC32B_W((LPWSTR)(STR), TRUE)

// ============================================================
// File extension
// ============================================================

ULONG FetchCurrentFileHash(
    IN LPWSTR szFilePath
)
{
    WCHAR* szDotIndex =
        wcsrchr(szFilePath, L'.');

    if (!szDotIndex ||
        szDotIndex == szFilePath)
    {
        return 0;
    }

    // +1 skips '.'
    return HASH_W(szDotIndex + 1);
}

// ============================================================
// Statistics
// ============================================================

volatile LONG64 g_TotalFilesFound = 0;
volatile LONG64 g_MatchingFiles = 0;

// ============================================================
// Recursive directory enumeration
// ============================================================

BOOL EnumerateDirectory(
    IN LPCWSTR szDirectory
)
{
    WCHAR szSearchPath[MAX_PATH] = { 0 };

    HRESULT hr =
        StringCchPrintfW(
            szSearchPath,
            MAX_PATH,
            L"%s\\*",
            szDirectory
        );

    if (FAILED(hr))
        return FALSE;

    WIN32_FIND_DATAW FindData;

    HANDLE hFind =
        FindFirstFileW(
            szSearchPath,
            &FindData
        );

    if (hFind == INVALID_HANDLE_VALUE)
    {
        // This can happen because of permissions, etc.
        return FALSE;
    }

    do
    {
        // ----------------------------------------------------
        // Skip "." and ".."
        // ----------------------------------------------------

        if (wcscmp(FindData.cFileName, L".") == 0 ||
            wcscmp(FindData.cFileName, L"..") == 0)
        {
            continue;
        }

        // ----------------------------------------------------
        // Build full path
        // ----------------------------------------------------

        WCHAR szFullPath[MAX_PATH] = { 0 };

        hr =
            StringCchPrintfW(
                szFullPath,
                MAX_PATH,
                L"%s\\%s",
                szDirectory,
                FindData.cFileName
            );

        if (FAILED(hr))
            continue;

        // ----------------------------------------------------
        // Directory
        // ----------------------------------------------------

        if (FindData.dwFileAttributes &
            FILE_ATTRIBUTE_DIRECTORY)
        {
            EnumerateDirectory(szFullPath);
        }

        // ----------------------------------------------------
        // File
        // ----------------------------------------------------

        else
        {
            InterlockedIncrement64(
                &g_TotalFilesFound
            );

            ULONG uExtensionHash =
                FetchCurrentFileHash(szFullPath);

            if (uExtensionHash == 0)
                continue;

            // ------------------------------------------------
            // Check extension against hash table
            // ------------------------------------------------

            if (IsHashInHashTable(uExtensionHash))
            {
                InterlockedIncrement64(
                    &g_MatchingFiles
                );

                wprintf(
                    L"[MATCH] %s\n",
                    szFullPath
                );
            }
        }

    } while (
        FindNextFileW(
            hFind,
            &FindData
        )
        );

    FindClose(hFind);

    return TRUE;
}

// ============================================================
// Main
// ============================================================

int wmain()
{
    wprintf(
        L"=============================================\n"
        L"        File Extension Hash Scanner\n"
        L"=============================================\n\n"
    );
    wprintf(L"[*] Press <Enter> To start ...");
    getchar();

    // --------------------------------------------------------
    // Extensions we want to find
    // --------------------------------------------------------

    ULONG uTargetExtensions[] =
    {
        HASH_W(L"pdf"),
        //HASH_W(L"txt"),
        HASH_W(L"md")
    };

    DWORD dwTargetExtensionsSize =
        sizeof(uTargetExtensions);

    // --------------------------------------------------------
    // Initialize hash table
    // --------------------------------------------------------

    InitializeHashTable();

    if (!InsertHashArrayInHashTable(
        uTargetExtensions,
        dwTargetExtensionsSize))
    {
        wprintf(
            L"[!] Failed to initialize hash table.\n"
        );

        return 1;
    }

    wprintf(
        L"[+] Target extensions:\n"
        L"    .pdf\n"
       // L"    .txt\n"
        L"    .md\n\n"
    );

    // --------------------------------------------------------
    // Get logical drives
    // --------------------------------------------------------

    DWORD dwDriveBitMask =
        GetLogicalDrives();

    if (!dwDriveBitMask)
    {
        wprintf(
            L"[!] GetLogicalDrives failed. Error: %lu\n",
            GetLastError()
        );

        return 1;
    }

    // --------------------------------------------------------
    // Enumerate drives
    // --------------------------------------------------------

    for (DWORD i = 0;
        i < MAX_DRIVES_NMBR;
        i++)
    {
        // Is this drive present?
        if (!(dwDriveBitMask & (1 << i)))
            continue;

        WCHAR szDrivePath[4] = { 0 };

        StringCchPrintfW(
            szDrivePath,
            4,
            L"%c:\\",
            L'A' + i
        );

        UINT DriveType =
            GetDriveTypeW(szDrivePath);

        // Only scan fixed and removable drives
        if (DriveType != DRIVE_FIXED &&
            DriveType != DRIVE_REMOVABLE)
        {
            continue;
        }

        wprintf(
            L"\n[+] Scanning drive %s\n",
            szDrivePath
        );

        EnumerateDirectory(
            szDrivePath
        );
    }

    // --------------------------------------------------------
    // Final recap
    // --------------------------------------------------------

    wprintf(
        L"\n"
        L"=============================================\n"
        L"                  RECAP\n"
        L"=============================================\n"
        L"Total files encountered : %lld\n"
        L"Matching files          : %lld\n"
        L"=============================================\n",
        g_TotalFilesFound,
        g_MatchingFiles
    );

    wprintf(
        L"\nPress <Enter> to quit..."
    );

    getchar();

    return 0;
}