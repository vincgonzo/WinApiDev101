# APCs and Alertable Waits on Windows

**Companions:** `alertable_wait.cpp` and `non_alertable_wait.cpp`  
**Navigation:** Training overview · Tool setup

## Learning objectives

Understand how ordinary user-mode Asynchronous Procedure Calls (APCs) are queued and delivered to an existing Windows thread.

This exercise compares two implementations:

- **Alertable wait:** An ordinary user APC executes when the target worker enters an alertable wait.
- **Non-alertable wait:** An ordinary user APC remains pending when the target worker enters a non-alertable wait and is discarded when the worker exits.

Both implementations queue exactly one APC using `QueueUserAPC`. The intentional delivery-rule difference is the second argument passed to `SleepEx`.

```cpp
SleepEx(2000, TRUE);  // Alertable wait
SleepEx(2000, FALSE); // Non-alertable wait
```

The key distinction is that **successful APC queuing does not guarantee callback execution**. An ordinary user APC requires an appropriate delivery opportunity on its target thread.

## Repository structure

```text
11_APCs_And_Alertable_Waits/
├── README.md
├── alertable_wait.cpp
└── non_alertable_wait.cpp
```

## Environment and build

Requirements: Windows 10 version 1607 or later, or Windows 11, with the x64 Windows SDK and C/C++ runtime.

Open an **x64 Native Tools Command Prompt** in the directory containing the source files.

### Build and run the alertable implementation

```bat
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc alertable_wait.cpp /Fe:alertable_wait.exe /link /DEBUG
alertable_wait.exe
alertable_wait.exe --auto
```

### Build and run the non-alertable implementation

```bat
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc non_alertable_wait.cpp /Fe:non_alertable_wait.exe /link /DEBUG
non_alertable_wait.exe
non_alertable_wait.exe --auto
```

For Visual Studio, create an x64 Console App, disable precompiled headers, replace the generated source with the corresponding file, and select C++17, `/Od`, `/Zi`, and linker `/DEBUG`. Keep the Console subsystem and the matching executable/PDB.

Neither program requires third-party packages or shared training headers. Both resolve `SetThreadDescription` dynamically from `KernelBase.dll` to support the stated Windows compatibility target.

### Execution modes

| Mode | Behaviour |
|---|---|
| Normal | Pauses at a controlled observation gate for inspection with Process Explorer or WinDbg. |
| `--auto` | Skips the interactive checkpoint and continues automatically. |

The automatic runs are designed to finish within the exercise's 20-second budget under normal scheduling. Entry and join waits are each capped at six seconds. The automatic gate has a six-second limit, while normal-mode gate inspection is capped at 120 seconds.

These are not hard real-time guarantees. Debugger pauses, system load, and scheduler delays can affect execution.

Both programs return process exit code `0` on success and `1` on an API failure or violated invariant.

## Experiment 1 — An ordinary APC runs on its alertable target

**Source:** [`alertable_wait.cpp`](alertable_wait.cpp)

### Purpose

Observe one ordinary user APC delivered on an existing worker—not on the queuing main thread and not on a newly created callback thread.

The callback, `BenignApc`, is a normal image-backed function compiled into the executable. It records its executing thread ID and increments a counter using Interlocked operations.

There is no payload, remote process, generated code, private executable memory, special user APC, or kernel APC experiment.

### Controlled sequence

1. Main names itself `alertable-main` and creates a worker named `alertable-target`.
2. The program prints the process ID (PID) and thread IDs (TIDs) in decimal and hexadecimal.
3. The worker signals actual entry before waiting on a non-alertable release gate.
4. Main observes the entry event before queuing exactly one APC.
5. Main verifies that the callback count remains zero while the gate is closed.
6. In normal mode, the program pauses so the target thread can be inspected and a callback breakpoint can be installed.
7. Press Enter to release the gate.
8. The worker enters `SleepEx(2000, TRUE)`, dispatches the pending callback, and returns from the wait.
9. The worker exits. Main joins it before reading the results or releasing the context and handles.

Waiting for actual worker entry before queuing the APC excludes the documented timing case in which an APC queued before a thread starts can be delivered when that thread begins execution.

### Expected results

| Observation | Expected value |
|---|---|
| APCs queued | 1 |
| Callback count at the closed gate | 0 |
| Final callback count | 1 |
| `SleepEx` return value | `WAIT_IO_COMPLETION` (`0x000000c0`) |
| Callback TID | Equal to target TID |
| Callback TID versus main TID | Different |
| Process exit code | 0 on success |

### Why the alertable wait can return early

A pending APC can execute as soon as the worker enters an alertable wait. The requested two-second sleep therefore does not necessarily last two seconds.

`WAIT_IO_COMPLETION` indicates that the alertable wait was interrupted by APC or I/O completion. It does not mean that the requested delay expired.

No exact elapsed time is asserted.

### Inspect with Process Explorer

At the release-gate checkpoint:

1. Open the process properties for the printed PID.
2. Select **Threads**.
3. Locate the worker using its printed decimal TID.
4. Inspect its stack.

The worker should be approaching or inside the event wait.

Do not expect a later GUI refresh to capture `BenignApc`. The callback is short-lived and can finish before the next refresh. Use WinDbg for direct evidence of callback execution.

### Inspect with Procmon

Capture process and thread activity from process startup.

Procmon can show the worker's creation and eventual exit. Queuing or delivering an APC does not create a new thread specifically for the callback.

- The worker is created by `CreateThread`.
- An APC callback executes within an existing target thread when delivered.
- Other unrelated process threads may exist.
- Procmon thread events are not a complete APC-delivery trace.

The recorded callback TID and a breakpoint identify the actual execution context more directly than a thread-creation snapshot.

### Inspect with WinDbg

Launch the normal executable under x64 WinDbg with its matching PDB.

```text
.symfix
.sympath+ C:\training\11_APCs_And_Alertable_Waits
.reload
x alertable_wait!*BenignApc*
bu alertable_wait!BenignApc
g
```

At the queued-but-gated console prompt, optionally break into the debugger and inspect the target before releasing it.

```text
~
~~[HEX_TID]s
k
g
```

Replace `HEX_TID` with the printed target TID in hexadecimal, without the `0x` prefix.

The `~` command displays debugger thread indexes, which are not OS TIDs. For example, `~2s` selects debugger thread index 2, while `~~[HEX_TID]s` selects a thread by its OS TID.

Continue with `g`, then press Enter in the console.

At the `BenignApc` breakpoint, inspect the executing context.

```text
~.
k
r rip
ln @rip
lm
!address @rip
dv /t
g
```

Expected evidence:

- The current OS TID matches the worker's TID.
- The instruction pointer resolves to the callback's image-backed code.
- The stack may include an APC dispatcher such as `ntdll!KiUserApcDispatcher`, wait-related frames, and `ApcTarget`.

Exact stack frames and unwind visibility vary by Windows build and available symbols.

If needed, also break on `KERNELBASE!SleepEx`.

```text
bu KERNELBASE!SleepEx
g
r ecx edx
k
```

At x64 function entry, ECX should contain `2000` and EDX should contain `1` (`TRUE`).

The callback breakpoint is the reliable moment to inspect the callback, not an assumption about a later stack snapshot.

## Experiment 2 — A queued APC does not run during an ordinary sleep

**Source:** [`non_alertable_wait.cpp`](non_alertable_wait.cpp)

### Purpose

Demonstrate that a successful `QueueUserAPC` call is not evidence that its callback executed.

Main queues one ordinary user APC to an already-started worker. The worker subsequently calls `SleepEx(2000, FALSE)`, never enters a later alertable wait, and exits.

The expected callback count is zero.

The callback is a benign, image-backed function compiled into the executable. It increments a counter and records the executing TID if called.

There is no remote process, payload, generated code, private executable memory, or special user APC.

### Controlled sequence

1. Main names itself `nonalertable-main` and creates a worker named `nonalertable-target`.
2. The program prints the PID and TIDs in decimal and hexadecimal.
3. The worker records its TID, signals the entry event, and waits on a non-alertable release gate.
4. Main observes the entry event and calls `QueueUserAPC` exactly once.
5. Main verifies that the callback count remains zero while the gate is closed.
6. In normal mode, the program pauses so the worker can be inspected.
7. Press Enter to open the gate.
8. The worker calls `SleepEx(2000, FALSE)`, records the return value, and exits without another wait.
9. Main joins the worker before reading the final record or releasing the context and handles.

Waiting for actual worker entry before queuing the APC excludes the documented pre-start delivery case.

### Expected results

| Observation | Expected value |
|---|---|
| APCs queued | 1 |
| Callback count at the closed gate | 0 |
| Final callback count | 0 |
| `SleepEx` return value | `0` |
| Callback TID | `0` (not recorded) |
| Worker exit code | 0 on success |

A callback TID of zero means that no callback execution was recorded. It is not the worker's actual TID.

The pending APC is discarded when the worker exits. Main does not cancel the APC.

API failures or violated program invariants result in an exit code of `1`.

### Inspect with Process Explorer

At the release-gate checkpoint:

1. Open the process properties for the printed PID.
2. Select **Threads**.
3. Identify the target using its decimal TID.
4. Inspect its stack.

The worker should be approaching or inside the gate's event wait. After release, it can instead be in `SleepEx` before exiting. Refresh timing may miss this brief phase entirely.

### Inspect with Procmon

Capture process and thread activity from startup.

Procmon can corroborate worker creation and exit, but it is not a complete APC-delivery trace.

The absence of another thread-creation event does not establish whether the callback executed. Use the final callback count, recorded TID, joined worker, and debugger breakpoint as the primary evidence.

### Inspect with WinDbg

Launch the normal executable under x64 WinDbg.

```text
.symfix
.sympath+ C:\training\11_APCs_And_Alertable_Waits
.reload
x non_alertable_wait!*BenignApc*
bu non_alertable_wait!BenignApc
g
```

At the queued-but-gated console prompt, break into the debugger.

```text
~
~~[HEX_TID]s
k
r rip
ln @rip
bu KERNELBASE!SleepEx
g
```

Replace `HEX_TID` with the worker's printed hexadecimal TID, without the `0x` prefix.

Continue the debugger before pressing Enter in the console.

At the `SleepEx` breakpoint, inspect the arguments and stack.

```text
r ecx edx
k
g
```

At x64 function entry:

- ECX should contain `2000` (`0x7d0`).
- EDX should contain `0`, representing `FALSE`.

The `BenignApc` breakpoint should not be reached during this run because the worker never becomes alertable.

If the Windows export resolves through a different symbol path, use:

```text
x KERNELBASE!*SleepEx*
```

Then set the breakpoint on the resolved function.

Debugger breaks perturb timing. A stale run can exceed the gate timeout, so restart the exercise if necessary.

## Direct comparison

| Property | Alertable wait | Non-alertable wait |
|---|---|---|
| Source file | `alertable_wait.cpp` | `non_alertable_wait.cpp` |
| Wait call | `SleepEx(2000, TRUE)` | `SleepEx(2000, FALSE)` |
| APCs queued | 1 | 1 |
| Worker entered before queuing | Yes | Yes |
| Callback executes | Yes | No |
| Final callback count | 1 | 0 |
| Wait return value | `WAIT_IO_COMPLETION` | `0` |
| Callback TID | Target TID | `0`, not recorded |
| Pending APC at worker exit | Already delivered | Discarded |

The examples use the same general worker and callback design to isolate the effect of the alertable-wait flag.

**Key takeaway:** An ordinary user APC can execute when its target thread enters an alertable wait. Queuing an APC successfully does not guarantee delivery, and an ordinary sleep is not an alertable delivery opportunity.

## Caveats and limitations

### APC delivery rules

These examples concern ordinary user APCs queued with `QueueUserAPC`. They do not demonstrate kernel APCs or special user APC mechanisms, which have different delivery rules.

### Thread and callback lifetime

The callback executes within an existing thread context. Production callback code must account for reentrancy, object lifetimes, synchronization, and which waits permit delivery.

Both examples join the worker before reading the final results or releasing the associated context and handles. The callback does not recursively enter another alertable wait.

### Timing and observation

- The requested two-second sleep is not an exact elapsed-time guarantee in the alertable example.
- Debugger pauses and scheduler delays can invalidate timing expectations.
- WPR/WPA CPU sampling may miss the entire callback because it is short-lived.
- Missing CPU samples do not establish non-delivery.
- Image-backed instructions establish code provenance, not a general behavioral verdict.
- User-mode commands such as `k`, `~`, and `!address` do not directly expose kernel `ETHREAD` or `KTHREAD` objects.

A callback breakpoint combined with the joined counter, recorded TID, and wait return value provides stronger evidence than a coarse stack snapshot alone.

## Learner questions

1. Why does the callback count remain zero while the release gate is closed in both examples?
2. Why can the alertable two-second sleep return almost immediately?
3. What does a successful `QueueUserAPC` call prove, and what does it not prove?
4. Why must main observe actual worker entry before queuing the APC?
5. Which observations establish that the callback ran on the target thread rather than on main?
6. Why is a callback breakpoint stronger evidence than a Process Explorer refresh?
7. What happens to the pending APC when the non-alertable worker exits?
8. Why does the absence of a new thread in Procmon tell you little about APC delivery?
9. How do the `SleepEx` return values differ between the two experiments?
10. How do ordinary user APCs differ from special user APCs and kernel APCs?

## Official references

- [QueueUserAPC](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-queueuserapc)
- [SleepEx](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleepex)
- [Asynchronous Procedure Calls](https://learn.microsoft.com/en-us/windows/win32/sync/asynchronous-procedure-calls)
- [InterlockedIncrement](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-interlockedincrement)
- [SetThreadDescription](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreaddescription)
- [Alertable I/O](https://learn.microsoft.com/en-us/windows/win32/fileio/alertable-i-o)
- [WinDbg thread syntax](https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/threads--s---)
- [x64 calling convention](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention)

## Key takeaway

**An APC is a callback queued to a thread, not a new thread.** The alertable example shows the callback executing on the existing target thread, while the non-alertable example shows that queuing alone is insufficient. Compare the callback count, recorded TID, wait return value, and debugger evidence to understand the distinction between queuing and execution.
