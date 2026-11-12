// InjectorWithAPC.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <Windows.h>
#include <stdio.h>
#include <vector>
#include <TlHelp32.h>

std::vector<DWORD> GetThreads(DWORD pid) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return {};

    THREADENTRY32 te;
    te.dwSize = sizeof(te);

    Thread32First(hSnapshot, &te);

    std::vector<DWORD> tids;
    while (Thread32Next(hSnapshot, &te)) {
        if (te.th32OwnerProcessID == pid)
            tids.push_back(te.th32ThreadID);
    }
    CloseHandle(hSnapshot);
    return tids;
}

int main(int argc, const char* argv[])
{
    if (argc < 3) {
        printf("Usage: InjectorWithAPC <pid> <dllpath>\n");
        return 0;
    }

    int pid = strtol(argv[1], nullptr, 0);
    HANDLE hProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
    if (!hProcess) {
        printf("Failed to open process (%u)\n", GetLastError());
        return 1;
    }

    auto threads = GetThreads(pid);
    if (threads.empty()) {
        printf("No Threads found\n");
        return 1;
    }

    void* buffer = VirtualAllocEx(hProcess, nullptr, strlen(argv[2]) + 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!buffer)
        return 1;
    if (!WriteProcessMemory(hProcess, buffer, argv[2], strlen(argv[2]), nullptr))
        return 1;

    auto f = (PAPCFUNC)GetProcAddress(GetModuleHandle(L"kernel32"), "LoadLibraryA");
    for (DWORD tid : threads) {
        HANDLE hThread = OpenThread(THREAD_SET_CONTEXT, FALSE, tid);
        if (hThread) {
            QueueUserAPC(f, hThread, (ULONG_PTR)buffer);
            printf("Queue APC to thread %u\n", tid);
            CloseHandle(hThread);
        }
    }

    CloseHandle(hProcess);
    return 0;
}
