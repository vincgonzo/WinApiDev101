// ProcessAndThreadEnum.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <windows.h>
#include <stdio.h>
#include <TlHelp32.h>
#include <wtsapi32.h>

#pragma comment(lib, "Wtsapi32.lib")        

void EnumProcessWithWTS() {
    DWORD level = 1;
    WTS_PROCESS_INFO_EX* data = nullptr;
    DWORD count;
    if (WTSEnumerateProcessesEx(WTS_CURRENT_SERVER_HANDLE, &level, WTS_ANY_SESSION, (LPWSTR*)&data, &count)) {
        for (DWORD i = 0; i < count; i++) {
            auto& p = data[i];
            printf("PID: %u Session: %u Handles: %u Name: %ws\n",
                p.ProcessId, p.SessionId, p.HandleCount, p.pProcessName);
        }
        WTSFreeMemoryEx(WTSTypeProcessInfoLevel1, data, count);
        data = nullptr;
    }
}

int main()
{
    EnumProcessWithWTS();
    return 0;
    HANDLE hToolHelp = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hToolHelp == INVALID_HANDLE_VALUE)
        return 1;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);

    Process32First(hToolHelp, &pe);
    do {
        printf("PID: %u PPID: %u Threads: %u Name: %ws\n",
            pe.th32ProcessID, pe.th32ParentProcessID, pe.cntThreads, pe.szExeFile);
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (hProcess) {
            WCHAR path[MAX_PATH];
            DWORD size = _countof(path);
            if (QueryFullProcessImageName(hProcess, 0, path, &size)) {
                printf("\tExe path: %ws\n", path);
            }
            CloseHandle(hProcess);
        }
    } while (Process32Next(hToolHelp, &pe));
    
    CloseHandle(hToolHelp);
    return 0;
}