#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <strsafe.h>
#include <stdint.h>

#define CRC_POLYNOMIAL  0xEDB88320u
#define HASH_TABLE_SIZE 1024u
#define NUMBER_OF_THREADS 64u

/* Hash-table occupancy is tracked separately, so hash value 0 is valid. */
typedef struct HASH_SLOT {
    ULONG value;
    BOOL occupied;
} HASH_SLOT;

static HASH_SLOT g_HashTable[HASH_TABLE_SIZE];
static CRITICAL_SECTION g_PrintLock;
static PTP_POOL g_Pool = NULL;
static TP_CALLBACK_ENVIRON g_CallbackEnv;
static PTP_CLEANUP_GROUP g_CleanupGroup = NULL;
static volatile LONG64 g_ActiveTasks = 0;

typedef enum SCAN_PHASE {
    PHASE_PRIORITY = 0,
    PHASE_SECONDARY = 1,
    PHASE_COUNT = 2
} SCAN_PHASE;

typedef struct PHASE_COUNTERS {
    volatile LONG64 files_seen;
    volatile LONG64 files_with_extension;
    volatile LONG64 files_matched;
    volatile LONG64 directories_seen;
    volatile LONG64 directories_failed;
} PHASE_COUNTERS;

static PHASE_COUNTERS g_Counters[PHASE_COUNT];

typedef struct WORK_ITEM {
    WCHAR *directory;
    SCAN_PHASE phase;
} WORK_ITEM;

static ULONG HashFunction(ULONG value)
{
    return value % HASH_TABLE_SIZE;
}

static ULONG CRC32W(const WCHAR *text, BOOL lowercase)
{
    ULONG crc = 0xFFFFFFFFu;
    if (!text) return 0;

    for (; *text; ++text) {
        WCHAR ch = *text;
        if (lowercase && ch >= L'A' && ch <= L'Z')
            ch = (WCHAR)(ch - L'A' + L'a');

        /* Keep the original program's WCHAR-oriented hashing behavior. */
        crc ^= (ULONG)ch;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? CRC_POLYNOMIAL : 0u);
    }
    return ~crc;
}

#define HASH_W(s) CRC32W((s), TRUE)

/* Returns FALSE only when the table is full or the input is invalid. */
static BOOL InsertToHashTable(ULONG hash)
{
    ULONG start = HashFunction(hash);
    for (ULONG n = 0; n < HASH_TABLE_SIZE; ++n) {
        ULONG index = (start + n) % HASH_TABLE_SIZE;
        if (g_HashTable[index].occupied) {
            if (g_HashTable[index].value == hash)
                return TRUE; /* duplicate is already present */
        } else {
            g_HashTable[index].value = hash;
            g_HashTable[index].occupied = TRUE;
            return TRUE;
        }
    }
    return FALSE;
}

/* lengthBytes is the byte size of the array, e.g. sizeof(array). */
static BOOL InsertHashArrayInHashTable(const ULONG *hashes, SIZE_T lengthBytes)
{
    if (!hashes || lengthBytes == 0 || lengthBytes % sizeof(hashes[0]) != 0)
        return FALSE;

    SIZE_T count = lengthBytes / sizeof(hashes[0]);
    for (SIZE_T i = 0; i < count; ++i) {
        if (!InsertToHashTable(hashes[i])) {
            fwprintf(stderr, L"[!] Hash table is full while inserting entry %llu\n",
                     (unsigned long long)i);
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL IsHashInHashTable(ULONG hash)
{
    ULONG start = HashFunction(hash);
    for (ULONG n = 0; n < HASH_TABLE_SIZE; ++n) {
        ULONG index = (start + n) % HASH_TABLE_SIZE;
        if (!g_HashTable[index].occupied)
            return FALSE; /* an empty slot terminates this probe sequence */
        if (g_HashTable[index].value == hash)
            return TRUE;
    }
    return FALSE;
}

/* Requested HashEmpty helper: TRUE when no hash has been inserted. */
static BOOL HashEmpty(void)
{
    for (ULONG i = 0; i < HASH_TABLE_SIZE; ++i)
        if (g_HashTable[i].occupied)
            return FALSE;
    return TRUE;
}

static void ClearHashTable(void)
{
    ZeroMemory(g_HashTable, sizeof(g_HashTable));
}

/* Hash only the final extension, never a dot in a parent directory. */
static BOOL FetchCurrentFileHash(const WCHAR *path, ULONG *outHash)
{
    if (!path || !outHash) return FALSE;

    const WCHAR *lastSlash = wcsrchr(path, L'\\');
    const WCHAR *lastSlash2 = wcsrchr(path, L'/');
    if (!lastSlash || (lastSlash2 && lastSlash2 > lastSlash)) lastSlash = lastSlash2;

    const WCHAR *dot = wcsrchr(path, L'.');
    if (!dot || dot == path || (lastSlash && dot < lastSlash + 1) || dot[1] == L'\0')
        return FALSE;

    *outHash = HASH_W(dot + 1);
    return TRUE;
}

static WCHAR *DuplicateString(const WCHAR *source)
{
    if (!source) return NULL;
    SIZE_T chars = wcslen(source) + 1;
    if (chars > SIZE_MAX / sizeof(WCHAR)) return NULL;
    WCHAR *copy = (WCHAR *)LocalAlloc(LPTR, chars * sizeof(WCHAR));
    if (!copy) return NULL;
    if (FAILED(StringCchCopyW(copy, chars, source))) {
        LocalFree(copy);
        return NULL;
    }
    return copy;
}

static VOID CALLBACK ScanDirectoryCallback(PTP_CALLBACK_INSTANCE instance,
                                            PVOID context,
                                            PTP_WORK work);

/* Increment active count BEFORE submitting, preventing the main thread from
   observing zero while a queued callback has not started yet. */
static BOOL EnqueueDirectory(const WCHAR *directory, SCAN_PHASE phase)
{
    WORK_ITEM *item = (WORK_ITEM *)LocalAlloc(LPTR, sizeof(*item));
    if (!item) return FALSE;

    item->directory = DuplicateString(directory);
    item->phase = phase;
    if (!item->directory) {
        LocalFree(item);
        return FALSE;
    }

    PTP_WORK work = CreateThreadpoolWork(ScanDirectoryCallback, item, &g_CallbackEnv);
    if (!work) {
        LocalFree(item->directory);
        LocalFree(item);
        return FALSE;
    }

    InterlockedIncrement64(&g_ActiveTasks);
    SubmitThreadpoolWork(work);
    return TRUE;
}

static VOID CALLBACK ScanDirectoryCallback(PTP_CALLBACK_INSTANCE instance,
                                            PVOID context,
                                            PTP_WORK work)
{
    UNREFERENCED_PARAMETER(instance);
    UNREFERENCED_PARAMETER(work);

    WORK_ITEM *item = (WORK_ITEM *)context;
    if (!item) {
        InterlockedDecrement64(&g_ActiveTasks);
        return;
    }

    SCAN_PHASE phase = item->phase;
    PHASE_COUNTERS *counters = &g_Counters[phase];
    WCHAR searchPath[MAX_PATH];
    WIN32_FIND_DATAW data;
    HANDLE find = INVALID_HANDLE_VALUE;

    InterlockedIncrement64(&counters->directories_seen);
    SIZE_T dirLen = wcslen(item->directory);
    HRESULT hr = (dirLen > 0 && item->directory[dirLen - 1] == L'\\')
        ? StringCchPrintfW(searchPath, ARRAYSIZE(searchPath), L"%s*", item->directory)
        : StringCchPrintfW(searchPath, ARRAYSIZE(searchPath), L"%s\\*", item->directory);
    if (FAILED(hr)) {
        InterlockedIncrement64(&counters->directories_failed);
        goto cleanup;
    }

    find = FindFirstFileW(searchPath, &data);
    if (find == INVALID_HANDLE_VALUE) {
        InterlockedIncrement64(&counters->directories_failed);
        goto cleanup;
    }

    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
            continue;

        WCHAR fullPath[MAX_PATH];
        dirLen = wcslen(item->directory);
        hr = (dirLen > 0 && item->directory[dirLen - 1] == L'\\')
            ? StringCchPrintfW(fullPath, ARRAYSIZE(fullPath), L"%s%s", item->directory, data.cFileName)
            : StringCchPrintfW(fullPath, ARRAYSIZE(fullPath), L"%s\\%s", item->directory, data.cFileName);
        if (FAILED(hr)) continue;

        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                continue; /* avoid junction/symlink loops */
            if (!EnqueueDirectory(fullPath, phase)) {
                InterlockedIncrement64(&counters->directories_failed);
                EnterCriticalSection(&g_PrintLock);
                fwprintf(stderr, L"[!] Could not queue directory: %s\n", fullPath);
                LeaveCriticalSection(&g_PrintLock);
            }
            continue;
        }

        InterlockedIncrement64(&counters->files_seen);
        ULONG extensionHash = 0;
        if (!FetchCurrentFileHash(fullPath, &extensionHash))
            continue;
        InterlockedIncrement64(&counters->files_with_extension);

        /* The hash table is immutable for the duration of each scan phase. */
        if (IsHashInHashTable(extensionHash)) {
            InterlockedIncrement64(&counters->files_matched);
#ifdef PRINT_FILE_NAME
            EnterCriticalSection(&g_PrintLock);
            wprintf(L"[MATCH] %s\n", fullPath);
            LeaveCriticalSection(&g_PrintLock);
#endif
        }
    } while (FindNextFileW(find, &data));

cleanup:
    if (find != INVALID_HANDLE_VALUE) FindClose(find);
    LocalFree(item->directory);
    LocalFree(item);
    InterlockedDecrement64(&g_ActiveTasks);
}

static BOOL InitializeThreadPool(void)
{
    InitializeThreadpoolEnvironment(&g_CallbackEnv);
    g_Pool = CreateThreadpool(NULL);
    if (!g_Pool) {
        fwprintf(stderr, L"[!] CreateThreadpool failed: %lu\n", GetLastError());
        DestroyThreadpoolEnvironment(&g_CallbackEnv);
        return FALSE;
    }

    SetThreadpoolThreadMaximum(g_Pool, NUMBER_OF_THREADS);
    if (!SetThreadpoolThreadMinimum(g_Pool, 1)) {
        fwprintf(stderr, L"[!] SetThreadpoolThreadMinimum failed: %lu\n", GetLastError());
        CloseThreadpool(g_Pool);
        g_Pool = NULL;
        DestroyThreadpoolEnvironment(&g_CallbackEnv);
        return FALSE;
    }

    g_CleanupGroup = CreateThreadpoolCleanupGroup();
    if (!g_CleanupGroup) {
        fwprintf(stderr, L"[!] CreateThreadpoolCleanupGroup failed: %lu\n", GetLastError());
        CloseThreadpool(g_Pool);
        g_Pool = NULL;
        DestroyThreadpoolEnvironment(&g_CallbackEnv);
        return FALSE;
    }

    SetThreadpoolCallbackPool(&g_CallbackEnv, g_Pool);
    SetThreadpoolCallbackCleanupGroup(&g_CallbackEnv, g_CleanupGroup, NULL);
    return TRUE;
}

static VOID DestroyThreadPool(void)
{
    if (g_CleanupGroup) {
        CloseThreadpoolCleanupGroupMembers(g_CleanupGroup, FALSE, NULL);
        CloseThreadpoolCleanupGroup(g_CleanupGroup);
        g_CleanupGroup = NULL;
    }
    DestroyThreadpoolEnvironment(&g_CallbackEnv);
    if (g_Pool) {
        CloseThreadpool(g_Pool);
        g_Pool = NULL;
    }
}

static VOID ResetPhaseCounters(SCAN_PHASE phase)
{
    ZeroMemory(&g_Counters[phase], sizeof(g_Counters[phase]));
}

static BOOL ScanAllFixedAndRemovableDrives(SCAN_PHASE phase)
{
    DWORD driveMask = GetLogicalDrives();
    if (!driveMask) {
        fwprintf(stderr, L"[!] GetLogicalDrives failed: %lu\n", GetLastError());
        return FALSE;
    }

    BOOL queuedAny = FALSE;
    for (DWORD i = 0; i < 26; ++i) {
        if (!(driveMask & (1u << i))) continue;

        WCHAR drive[] = L"A:\\";
        drive[0] = (WCHAR)(L'A' + i);
        UINT type = GetDriveTypeW(drive);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;

        if (!EnqueueDirectory(drive, phase)) {
            fwprintf(stderr, L"[!] Failed to queue drive %s\n", drive);
            return FALSE;
        }
        queuedAny = TRUE;
    }

    if (!queuedAny)
        fwprintf(stderr, L"[!] No fixed or removable drives were found.\n");

    while (InterlockedCompareExchange64(&g_ActiveTasks, 0, 0) != 0)
        Sleep(50);

    /* Close completed work objects; the cleanup group remains reusable. */
    CloseThreadpoolCleanupGroupMembers(g_CleanupGroup, FALSE, NULL);
    return queuedAny;
}

static VOID PrintPhaseRecap(const WCHAR *label, SCAN_PHASE phase)
{
    const PHASE_COUNTERS *c = &g_Counters[phase];
    wprintf(L"\n%s\n", label);
    wprintf(L"  Directories visited       : %lld\n", c->directories_seen);
    wprintf(L"  Directory errors          : %lld\n", c->directories_failed);
    wprintf(L"  Files encountered         : %lld\n", c->files_seen);
    wprintf(L"  Files with an extension   : %lld\n", c->files_with_extension);
    wprintf(L"  Matching files            : %lld\n", c->files_matched);
}

int wmain(void)
{
    LARGE_INTEGER frequency, start, end;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    InitializeCriticalSection(&g_PrintLock);

    wprintf(L"=============================================\n"
            L"       File Extension Hash Scanner\n"
            L"=============================================\n\n"
            L"[*] Press Enter to start ...");
    (void)getwchar();

    if (!InitializeThreadPool()) {
        DeleteCriticalSection(&g_PrintLock);
        return 1;
    }

    /* Phase 1: extensions to prioritize. */
    const ULONG priorityHashes[] = {
        HASH_W(L"pdf"), HASH_W(L"txt"), HASH_W(L"md")
    };
    ClearHashTable();
    ResetPhaseCounters(PHASE_PRIORITY);
    BOOL ok = InsertHashArrayInHashTable(priorityHashes, sizeof(priorityHashes));
    if (!ok || HashEmpty()) {
        fwprintf(stderr, L"[!] Priority hash table initialization failed.\n");
        DestroyThreadPool();
        DeleteCriticalSection(&g_PrintLock);
        return 1;
    }
    wprintf(L"[i] Scanning priority extensions ...\n");
    BOOL priorityScanOk = ScanAllFixedAndRemovableDrives(PHASE_PRIORITY);

    /* Phase 2: the secondary extension list, counted independently. */
    const ULONG secondaryHashes[] = {
        HASH_W(L"png"), HASH_W(L"jpg"), HASH_W(L"icon")
    };
    ClearHashTable();
    ResetPhaseCounters(PHASE_SECONDARY);
    ok = InsertHashArrayInHashTable(secondaryHashes, sizeof(secondaryHashes));
    if (!ok || HashEmpty()) {
        fwprintf(stderr, L"[!] Secondary hash table initialization failed.\n");
        DestroyThreadPool();
        DeleteCriticalSection(&g_PrintLock);
        return 1;
    }
    wprintf(L"\n[i] Scanning secondary extensions ...\n");
    BOOL secondaryScanOk = ScanAllFixedAndRemovableDrives(PHASE_SECONDARY);

    DestroyThreadPool();
    DeleteCriticalSection(&g_PrintLock);
    QueryPerformanceCounter(&end);

    double elapsed = (double)(end.QuadPart - start.QuadPart) / (double)frequency.QuadPart;
    wprintf(L"\n=============================================\n"
            L"                    RECAP\n"
            L"=============================================\n");
    PrintPhaseRecap(L"Priority extensions (pdf, txt, md):", PHASE_PRIORITY);
    PrintPhaseRecap(L"Secondary extensions (png, jpg, icon):", PHASE_SECONDARY);
    wprintf(L"\nCombined files encountered : %lld\n",
            g_Counters[PHASE_PRIORITY].files_seen + g_Counters[PHASE_SECONDARY].files_seen);
    wprintf(L"Combined matching results  : %lld\n",
            g_Counters[PHASE_PRIORITY].files_matched + g_Counters[PHASE_SECONDARY].files_matched);
    wprintf(L"Execution time             : %.3f seconds\n", elapsed);
    wprintf(L"Scan status                : %s\n",
            (priorityScanOk && secondaryScanOk) ? L"completed" : L"completed with errors");
    wprintf(L"=============================================\n\n"
            L"Press Enter to quit ...");
    (void)getwchar();
    return (priorityScanOk && secondaryScanOk) ? 0 : 1;
}
