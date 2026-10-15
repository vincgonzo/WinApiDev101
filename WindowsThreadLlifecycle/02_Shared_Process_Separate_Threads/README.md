# Shared Process, Separate Threads

## Objective

Run two live threads in one process and distinguish shared address space from
per-thread state. Both workers increment the same `Interlocked` counter, but
each has its own stack local, TLS payload, TEB, and last-error value. Main
deliberately reads the published worker-local addresses: stacks and TLS are
not security boundaries inside a process.

Start with [the bundle guide](../README.md) and [tool setup](../TOOLS.md).
This file describes only `shared_state_and_tls.cpp`; it has no dependencies
on another source file or a shared lab library.

## Build And Run

Use an **x64 Native Tools Command Prompt for Visual Studio** with the Windows
SDK installed, on Windows 10 version 1607 or later, or Windows 11 x64:

```bat
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc shared_state_and_tls.cpp /Fe:shared_state_and_tls.exe /link /DEBUG
shared_state_and_tls.exe
shared_state_and_tls.exe --auto
```

No arguments selects a console inspection checkpoint of up to 30 seconds.
Press Enter to finish sooner. Redirected/non-console input skips it. `--auto`
skips that checkpoint and normally finishes in well under 20 seconds. Any
other argument combination is rejected. Exit status is 0 for verified
success or 1 for an API, timeout, usage, or invariant failure. There is no
hardware-dependent status 77 case in this example.

`SetThreadDescription` is resolved from KernelBase at runtime to support
Windows 10 1607, whose export needs runtime dynamic linking. Naming failure
is reported, not silently ignored. Workers use `_beginthreadex` because
their error paths use the CRT. No elevation or driver is required.

## Phases And Falsifiable Checks

1. Main prints the PID and main TID in decimal and hex, allocates one TLS
   index, and creates workers named `shared-state-A` and `shared-state-B`.
   Each worker installs its own pointer at that same TLS index and captures
   its TID, TEB, stack bounds, stack addresses, and values.
2. The two prepared events acknowledge initialization. Main then releases a
   common start event. The workers each execute 100,000
   `InterlockedIncrement` operations on the same counter. They need not be
   simultaneously Running; their lifetimes overlap and scheduling remains
   up to Windows.
3. Each worker signals its ready event and approaches an event wait. Only
   after **both** ready events are observed does main read the captures.
   The expected final counter is exactly 200,000. Worker A/B locals are
   1000/1001, TLS payloads are `0xA1100000`/`0xA1100001`, and immediate
   last-error captures are `0xE1000000`/`0xE1000001`.
4. During the checkpoint, `SharedStateWorker` is still on both worker
   stacks. The local and TLS payload addresses remain valid. Main checks
   their contents, distinct TEB/local/TLS addresses, matching TIDs, and that
   each ordinary local lies within its captured NT_TIB bounds. A ready
   event proves publication, not that the worker has already been scheduled
   into its final wait.
5. Main signals release, joins both workers, verifies their zero exit codes,
   and only then frees the TLS index and closes every owned handle.

`volatile` on the counter exists for the Win32 Interlocked interface;
**Interlocked operations**, not `volatile`, synchronize all concurrent
counter access. The manual-reset events publish the other fields. Workers
do not change them while main inspects them, and main never dereferences
the stack pointers after releasing the workers.

## Process Explorer

1. Prepare Process Explorer before starting the executable. Locate the
   printed decimal PID and open **Properties > Threads**.
2. Match the printed worker TIDs, not their order in the table. The names
   may be displayed when supported by the tool version. At the inspection
   prompt, select each worker and open **Stack**. With matching executable
   PDBs and Microsoft symbols configured, look for `SharedStateWorker`
   above the event-wait path.
3. The process has one address space and one process-level resource set.
   Different TIDs and stacks do not mean separate processes. CPU activity
   during the short increment phase may be missed by a periodic sample;
   most of the observation checkpoint is waiting.

Do not infer exact kernel thread states from a stale stack snapshot. Tool
attachment and stack collection can also change the observed execution.

## WinDbg (User Mode, x64)

Launch under WinDbg, continue to the printed checkpoint, then break in. Add
the executable/PDB directory to the symbol path and use Microsoft symbols
as described in the tool guide. Useful commands are:

```text
.symfix
.sympath+ C:\path\to\this\lab
.reload /f
|
~
~~[1a2c]s
k
!teb
!peb
dt ntdll!_TEB @$teb
dt ntdll!_NT_TIB @$teb
dv /t /v
dd 0000012345678000 L1
g
```

Replace `1a2c` with a worker's **printed hexadecimal TID**, without the
`0x` prefix, and replace the `dd` address with a printed local or TLS
payload address. `~` displays debugger thread **indices** as well as
process/thread IDs; `~1s` selects index 1, whereas `~~[1a2c]s` selects an OS
TID. They are not interchangeable. `dv` needs the appropriate frame
selected (`.frame <number>` after `k`) and usable local symbols.

Select the other worker and repeat. `!teb` and `dt` should agree with the
printed TEB and NT_TIB stack bounds at this checkpoint. `!peb` describes
the same process for both selections. Inspect a printed TLS payload with
`dd`; do not assume every `TlsAlloc` index resides in an inline TEB array,
and do not hardcode private TEB offsets.

These are user-mode commands. Kernel `!thread` is not a command to run in
this user-mode session; kernel scheduling analysis requires a separately
configured kernel debugger or, more conveniently here, a scheduler trace.

## WPA Or Command-Line Alternative

For a scheduling view, start a WPR **GeneralProfile** recording before
launching the program and stop after it exits:

```bat
wpr -start GeneralProfile -filemode
shared_state_and_tls.exe
wpr -stop shared-state.etl
wpa shared-state.etl
```

Use a suitably authorized terminal if WPR requires elevation; avoid
interrupting someone else's existing recording. In WPA, filter **CPU
Usage (Precise)** by the printed PID and TIDs. Verify that the selected
profile collected context-switch/ready-thread events if that graph is
unavailable. The small increment burst may occupy little trace time.
Process Monitor thread creation/exit events can corroborate lifetimes but
are not a complete scheduler trace.

## Interpretation And Caveats

- `TlsAlloc` allocates an index for the process. `TlsSetValue` assigns a
  different value to that index for the calling thread. The payload here
  happens to be on the worker's stack; TLS need not point to stack memory.
- `SetLastError` is followed **immediately** by `GetLastError`, with no
  intervening API call. The stored capture is evidence of that instant
  only. `TlsGetValue` has its own immediate capture and clears last-error
  on success. Subsequent event or wait calls may change the current TEB
  last-error field, so a later `!teb` need not show the earlier test value.
- `NtCurrentTeb()` supplies the current TEB pointer. The code uses the
  Windows SDK's `NT_TIB` and its `StackLimit`/`StackBase` fields, with no
  handwritten offsets. These are current stack bounds, not a full map of
  reserved, committed, and guard pages. A deeper stack can grow.
- These checks prove independent per-thread storage conventions, not
  access isolation. Another thread in this process can read or corrupt
  those addresses. Do not modify them in the debugger during validation.
- Worker gate waits, release waits, and joins are bounded. Worker release
  timeouts are 45 seconds normally or 8 seconds in auto mode. A long
  debugger stop can outlast them; rerun rather than interpreting that as
  a TLS defect. Deadline checks are not hard real-time scheduling guarantees.
- On a fatal error the program reports it and exits the process with 1,
  letting Windows reclaim handles and threads. It does not use
  `TerminateThread`, free storage under live workers, or leave workers in
  another process. Native Windows runtime behavior must be verified there;
  cross-compilation alone does not establish it.

## Learner Questions

1. Why does one TLS index produce two different pointer values, while both
   workers see the same counter address?
2. What would become invalid if main released and joined the workers before
   dereferencing the published local addresses?
3. Why might the debugger's current `LastErrorValue` differ from the printed
   immediate capture even though the check passed?

## Microsoft References

- [Thread Local Storage](https://learn.microsoft.com/en-us/windows/win32/procthread/thread-local-storage)
- [TlsGetValue and its last-error behavior](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsgetvalue)
- [GetLastError](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-getlasterror)
- [NtCurrentTeb](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-ntcurrentteb)
- [TEB structure and compatibility warning](https://learn.microsoft.com/en-us/windows/win32/api/winternl/ns-winternl-teb)
- [InterlockedIncrement](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-interlockedincrement)
- [_beginthreadex](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/beginthread-beginthreadex)
- [SetThreadDescription and Windows 10 1607 runtime linking](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreaddescription)
- [WinDbg thread syntax](https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/thread-syntax)
- [CPU analysis in WPA](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/cpu-analysis)