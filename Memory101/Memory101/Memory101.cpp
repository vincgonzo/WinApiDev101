// Memory101.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <Windows.h>
#include <stdio.h>

int cell_size = 1024, sizex = 1024, sizey = 1024;
int main()
{
    BYTE* buffer = (BYTE*) VirtualAlloc(nullptr, sizex * sizey * cell_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    char text[] = "Hello, memory!";

    VirtualAlloc(buffer, cell_size, MEM_COMMIT, PAGE_READWRITE);
    memcpy(buffer, text, sizeof(text));

    return 0;
}
