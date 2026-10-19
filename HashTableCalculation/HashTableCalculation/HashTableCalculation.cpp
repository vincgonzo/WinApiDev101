#include <windows.h>
#include <stdio.h>
#include <strsafe.h>

#define CRC_POLYNOMIAL  0xEDB88320
#define EMPTY_SLOT      0
#define HASH_TABLE_SIZE 1024
#define MAX_DRIVES_NMBR 26

ULONG g_HashTable[HASH_TABLE_SIZE] = { 0 };
volatile LONG64 g_TotalFilesFound = 0;
volatile LONG64 g_MatchingFiles = 0;

SIZE_T HashFunction(ULONG uHash) {
    return uHash % HASH_TABLE_SIZE;
}

VOID InitializeHashTable(VOID) {
    for (DWORD i = 0; i < HASH_TABLE_SIZE; i++)
        g_HashTable[i] = EMPTY_SLOT;
}

BOOL InsertToHashTable(ULONG uHash) {
    SIZE_T Index = HashFunction(uHash);

    while (g_HashTable[Index] != EMPTY_SLOT && g_HashTable[Index] != uHash)
        Index = (Index + 1) % HASH_TABLE_SIZE;

    if (g_HashTable[Index] == uHash) return FALSE;

    g_HashTable[Index] = uHash;
    return TRUE;
}

BOOL InsertHashArrayInHashTable(ULONG uHashArray[], DWORD dwHashArrayLength) {
    if (!uHashArray || !dwHashArrayLength) return FALSE;

    DWORD Count = dwHashArrayLength / sizeof(ULONG);

    for (DWORD i = 0; i < Count; i++)
        if (!InsertToHashTable(uHashArray[i])) return FALSE;

    return TRUE;
}

BOOL IsHashInHashTable(ULONG uHash) {
    SIZE_T Index = HashFunction(uHash);

    while (g_HashTable[Index] != EMPTY_SLOT) {
        if (g_HashTable[Index] == uHash) return TRUE;
        Index = (Index + 1) % HASH_TABLE_SIZE;
    }

    return FALSE;
}

UINT32 CRC32B_W(WCHAR* szString, BOOL bLowerCaseHash) {
    UINT32 uMask = 0, uHash = 0xFFFFEFFFF;
    INT i = 0;

    if (!szString) return 0;

    if (bLowerCaseHash) {
        WCHAR szLowerCaseStr[MAX_PATH] = { 0 };

        for (int x = 0; szString[x] && x < MAX_PATH - 1; x++)
            szLowerCaseStr[x] = (szString[x] >= L'A' && szString[x] <= L'Z')
            ? szString[x] - L'A' + L'a' : szString[x];

        return CRC32B_W(szLowerCaseStr, FALSE);
    }

    while (szString[i] != 0) {
        uHash ^= (UINT32)szString[i];

        for (int k = 0; k < 8; k++) {
            uMask = -1 * (uHash & 1);
            uHash = (uHash >> 1) ^ (CRC_POLYNOMIAL & uMask);
        }

        i++;
    }

    return ~uHash;
}

#define HASH_W(STR) CRC32B_W((LPWSTR)(STR), TRUE)

ULONG FetchCurrentFileHash(LPWSTR szFilePath) {
    WCHAR* szDotIndex = wcsrchr(szFilePath, L'.');

    if (!szDotIndex || szDotIndex == szFilePath) return 0;

    return HASH_W(szDotIndex + 1);
}

BOOL EnumerateDirectory(LPCWSTR szDirectory) {
    WCHAR szSearchPath[MAX_PATH] = { 0 };

    if (FAILED(StringCchPrintfW(szSearchPath, MAX_PATH, L"%s\\*", szDirectory)))
        return FALSE;

    WIN32_FIND_DATAW FindData;
    HANDLE hFind = FindFirstFileW(szSearchPath, &FindData);

    if (hFind == INVALID_HANDLE_VALUE) return FALSE;

    do {
        if (!wcscmp(FindData.cFileName, L".") || !wcscmp(FindData.cFileName, L".."))
            continue;

        WCHAR szFullPath[MAX_PATH] = { 0 };

        if (FAILED(StringCchPrintfW(szFullPath, MAX_PATH, L"%s\\%s", szDirectory, FindData.cFileName)))
            continue;

        if (FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            EnumerateDirectory(szFullPath);
        }
        else {
            InterlockedIncrement64(&g_TotalFilesFound);

            ULONG uExtensionHash = FetchCurrentFileHash(szFullPath);

            if (!uExtensionHash) continue;

            if (IsHashInHashTable(uExtensionHash)) {
                InterlockedIncrement64(&g_MatchingFiles);
                wprintf(L"[MATCH] %s\n", szFullPath);
            }
        }
    } while (FindNextFileW(hFind, &FindData));

    FindClose(hFind);
    return TRUE;
}

int wmain(VOID) {
    LARGE_INTEGER Frequency, StartTime, EndTime;
    QueryPerformanceFrequency(&Frequency);
    QueryPerformanceCounter(&StartTime);
    wprintf(L"=============================================\n"
        L"        File Extension Hash Scanner\n"
        L"=============================================\n\n"
        L"[*] Press <Enter> To start ...");
    getchar();

    ULONG uTargetExtensions[] = { HASH_W(L"pdf"), HASH_W(L"md") };

    InitializeHashTable();

    if (!InsertHashArrayInHashTable(uTargetExtensions, sizeof(uTargetExtensions))) {
        wprintf(L"[!] Failed to initialize hash table.\n");
        return 1;
    }

    wprintf(L"[+] Target extensions:\n    .pdf\n    .md\n\n");

    DWORD dwDriveBitMask = GetLogicalDrives();

    if (!dwDriveBitMask) {
        wprintf(L"[!] GetLogicalDrives failed. Error: %lu\n", GetLastError());
        return 1;
    }

    for (DWORD i = 0; i < MAX_DRIVES_NMBR; i++) {
        if (!(dwDriveBitMask & (1 << i))) continue;

        WCHAR szDrivePath[4] = { 0 };
        StringCchPrintfW(szDrivePath, 4, L"%c:\\", L'A' + i);

        UINT DriveType = GetDriveTypeW(szDrivePath);

        if (DriveType != DRIVE_FIXED && DriveType != DRIVE_REMOVABLE)
            continue;

        wprintf(L"\n[+] Scanning drive %s\n", szDrivePath);
        EnumerateDirectory(szDrivePath);
    }

    QueryPerformanceCounter(&EndTime);

    double ElapsedTime = (double)(EndTime.QuadPart - StartTime.QuadPart) /
        Frequency.QuadPart; // timer end

    wprintf(L"\n=============================================\n"
        L"                  RECAP\n"
        L"=============================================\n"
        L"Total files encountered : %lld\n"
        L"Matching files          : %lld\n"
        L"Execution time          : %.3f seconds\n"
        L"=============================================\n",
        g_TotalFilesFound, g_MatchingFiles, ElapsedTime);

    wprintf(L"\nPress <Enter> to quit...");
    getchar();

    return 0;
}