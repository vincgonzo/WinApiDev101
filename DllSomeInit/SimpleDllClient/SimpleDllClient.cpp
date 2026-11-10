#include <Windows.h>
#include <iostream>

typedef int (*AddFunction)(int, int);

int main()
{
    HMODULE hModule = LoadLibraryA("DllSomeInit.dll");

    if (!hModule)   
    {
        std::cout << "Failed to load DLL: "
            << GetLastError() << std::endl;
        return 1;
    }

    AddFunction Add = (AddFunction)GetProcAddress(hModule, "Add");

    if (!Add)
    {
        std::cout << "Failed to find Add: "
            << GetLastError() << std::endl;

        FreeLibrary(hModule);
        return 1;
    }

    std::cout << "10 + 20 = " << Add(10, 20) << std::endl;

    FreeLibrary(hModule);

    return 0;
}