# A start address is not the current instruction

Companion: [start_address_and_stack.cpp](start_address_and_stack.cpp).
Navigation: [training overview](../README.md), [tool setup](../TOOLS.md).

## Purpose

Compare four different observations of one thread: the logical user function
passed to `CreateThread`, the Windows-reported Win32 start address displayed by a
tool, the current instruction pointer, and the complete current call stack.
These answer different questions. None, by itself, describes everything a
thread has done or will do.

`WorkerEntry` calls `WorkerOuter`, `WorkerMiddle`, and `WorkerWaitAtDepth`. All
four functions are explicitly non-inlined, and the build disables optimization.
The deepest function signals an entry event and then waits on a release event.
The worker uses Win32 APIs and integer/control operations only, not CRT calls;
this is an intentional `CreateThread` example.

## Build and run

Use Windows 10 version 1607 or later, or Windows 11, with an x64 Windows SDK and
the MSVC CRT. From an x64 Native Tools Command Prompt in this directory:

```bat
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc start_address_and_stack.cpp /Fe:start_address_and_stack.exe /link /DEBUG
start_address_and_stack.exe
start_address_and_stack.exe --auto
```

In Visual Studio, create an x64 **Console App**, disable precompiled headers,
remove the generated source, and add only this source. Select C++17, `/Od`,
`/Zi`, and linker `/DEBUG`; keep the Console subsystem. No shared training
headers or third-party dependencies are needed. Keep the EXE and matching PDB
together. Optimized builds can remove or reshape the stack evidence.
Thread naming resolves `SetThreadDescription` from `KernelBase.dll` at runtime,
as required on Windows 10 version 1607; it is not imported from newer Kernel32.

## Phases and evidence

1. Main names itself `stack-main`, prints the PID and main TID in decimal and
   hexadecimal, and prints the addresses of the four functions.
2. The named `stack-worker` reaches the deepest helper and signals its entry.
   Main prints its TID and holds an observation checkpoint. This event proves
   helper entry, not that the subsequent kernel wait has already started.
3. Enter releases the worker cooperatively. Main joins it, checks its exit code,
   and closes the handles. Success is process exit 0; failures exit 1.

Normal inspection has a 120-second worker safety timeout. `--auto` skips the
prompt; entry and join waits are each limited to six seconds, so the ordinary
automatic path is well below 20 seconds. This is not a real-time scheduling
guarantee. Long debugger stops can invalidate timeout-based observations.

## Process Explorer

Run normally, open the printed PID's **Properties > Threads**, and find the
worker by its decimal TID or description. Record **Start Address**, then use
**Stack** for the same thread. Configure symbols as described in
[tool setup](../TOOLS.md). Insufficient access or missing symbols can limit the
view; a raw address is not a different behavior.

For this direct Win32 creation path, the reported start address may resolve to
`WorkerEntry`. Other runtimes or creation paths can report a startup wrapper.
It is not necessarily the first instruction that actually executed in user
mode, and it does not move every time the thread calls another function.
The current stack should show the helper chain below the event-wait frames.
Windows frames such as `BaseThreadInitThunk` and `RtlUserThreadStart` can also
appear. Exact wrappers and displayed symbols depend on the Windows build.

## WinDbg: observe both entry and waiting

Launch the normal executable under x64 WinDbg. At the initial break, substitute
the directory containing your EXE and PDB:

```text
.symfix
.sympath+ C:\training\09_Start_Address_And_Current_Stack
.reload
x start_address_and_stack!*Worker*
bu start_address_and_stack!WorkerWaitAtDepth
g
```

At that breakpoint, `k` demonstrates the actual helper call chain and `r rip`
shows an instruction in the helper. Use `g` to reach the console checkpoint,
then break into the debugger and select the worker:

```text
~
~~[HEX_TID]s
k
r rip
ln @rip
lm
!address @rip
```

Replace `HEX_TID` with the printed hexadecimal worker TID without `0x`.
`~` lists debugger thread indexes as well as OS IDs; `~3s` selects debugger
index 3, not OS TID 3. `~~[HEX_TID]s` selects the OS thread explicitly.
While waiting, RIP often belongs to a Windows wait implementation rather than
`WorkerEntry`. `ln @rip`, `lm`, and `!address @rip` identify its symbol, loaded
module, and memory region. Repeat `!address` with a printed helper address to
compare its executable image mapping. Continue with `g` before pressing Enter
in the console.

## Interpretation and optional traces

An image-backed address establishes memory provenance, not whether the
surrounding behavior is benign or suspicious. All executable code here is
compiled image code; there is no generated code or private executable memory.
This lesson is useful when a thread's apparent origin is being confused with
its current workload. User-mode debugging does not expose a live ETHREAD or
KTHREAD through these commands; that requires an appropriate kernel session.

An optional WPR/WPA CPU trace can corroborate sampled stacks, but a waiting
thread may have few or no CPU samples. Procmon thread-create events describe
lifecycle, not the instruction stream. A coarse snapshot cannot prove every
helper executed; the entry breakpoint and event provide more precise evidence.

## Learner questions

- Which address remains associated with creation while RIP changes?
- Why does the entry event prove more than a delay in main would?
- What disappears if the call chain is inlined or tail-call optimized?
- Why is an executable image mapping insufficient to classify behavior?

## Official references

- [CreateThread](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-createthread)
- [WaitForSingleObject](https://learn.microsoft.com/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject)
- [SetThreadDescription](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreaddescription)
- [NtQueryInformationThread and ThreadQuerySetWin32StartAddress](https://learn.microsoft.com/windows/win32/api/winternl/nf-winternl-ntqueryinformationthread)
- [WinDbg thread syntax](https://learn.microsoft.com/windows-hardware/drivers/debuggercmds/thread-syntax)
- [WinDbg !address](https://learn.microsoft.com/windows-hardware/drivers/debuggercmds/-address)