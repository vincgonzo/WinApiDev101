#include <stdio.h>
#include <windows.h>


void PrintTEBInfo()
{
	// Structure definition for the TEB (Thread Environment Block)
	typedef struct _TEB {
		PVOID Reserved1[12];
		PVOID ProcessEnvironmentBlock;
		PVOID Reserved2[399];
		BYTE Reserved3[1952];
		PVOID TlsSlots[64];
		BYTE Reserved4[8];
		PVOID Reserved5[26];
		PVOID ReservedForOle; // Windows 2000 only
		PVOID Reserved6[4];
		PVOID TlsExpansionSlots;
	} TEB, * PTEB;
	PTEB teb;

#ifdef _M_X64 // x64 architecture
	// Access TEB in x64 using the GS segment register
	teb = (PTEB)__readgsqword(0x30);
#else // x86 architecture
	// Access TEB in x86 using the FS segment register
	__asm {
		mov eax, fs: [0x18]
		mov teb, eax
	}
#endif
	// Print some of the TEB fields
	printf("TEB address: 0x%p\n", teb);
	printf("Process Environment Block (PEB) address: 0x%p\n", teb->ProcessEnvironmentBlock);
	printf("TLS Expansion Slots address: 0x%p\n", teb->TlsExpansionSlots);
}


int main()
{
	PrintTEBInfo();
	return 0;
}