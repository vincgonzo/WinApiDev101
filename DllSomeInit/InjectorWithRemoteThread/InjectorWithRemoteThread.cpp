// InjectorWithRemoteThread.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <Windows.h>
#include <stdio.h>

int main(int argc, const char* argv[])
{
    if (argc < 3) {
        printf("Usage: InjectorWithRemoteThread <pid> <dllpath>\n");
        return 0;
    }

    int pid = strtol(argv[1], nullptr, 0);
    HANDLE hProcess = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
    if (!hProcess) {
        printf("Failed to open process (%u)\n", GetLastError());
        return 1;
    }


    void* buffer = VirtualAllocEx(hProcess, nullptr, strlen(argv[2]) + 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!buffer)
        return 1;
    if(!WriteProcessMemory(hProcess, buffer, argv[2], strlen(argv[2]), nullptr))
        return 1;

    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0, (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandle(L"kernel32"), "LoadLibraryA"), buffer, 0, nullptr);
    if (!hThread)
        return 1;

    WaitForSingleObject(hThread, INFINITE);
    printf("Successfully injected!\n");
    VirtualFreeEx(hProcess, buffer, 0, MEM_RELEASE);

    CloseHandle(hThread);
    CloseHandle(hProcess);
}
