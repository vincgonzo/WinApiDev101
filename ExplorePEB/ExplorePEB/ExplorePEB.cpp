// ExplorePEB.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <windows.h>
#include <stdio.h>
#include <winternl.h>

int main()
{
    PPEB peb = NtCurrentTeb()->ProcessEnvironmentBlock;

    LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;
    
    for (LIST_ENTRY* next = head->Flink; next != head; next = next->Flink) {
        auto entry = CONTAINING_RECORD(next, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        printf("0x%p: %wZ\n", entry->DllBase, entry->FullDllName); //Z for Unicode struct str
    }
    return 0;
}
