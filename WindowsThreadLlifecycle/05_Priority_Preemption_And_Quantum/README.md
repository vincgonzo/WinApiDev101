# Windows Thread Scheduling: Priority, Contention, and Preemption

This lab compares two Windows thread-scheduling experiments:

- **Equal configured priority:** two CPU-bound workers compete on one eligible logical processor with the same configured priority.
- **Higher-priority wake-up:** a normal-priority worker computes while an above-normal-priority worker waits, then becomes runnable.

The goal is to understand the relationship between configured priority, processor affinity, execution time, readiness, waiting, and preemption. Neither experiment is a benchmark or a guarantee of a specific scheduling outcome.

See [`TOOLS.md`](TOOLS.md) and the training overview for shared setup instructions.

## Requirements

- Windows 10 version 1607 or later, or Windows 11, x64.
- An x64 Native Tools Command Prompt with the Windows SDK.
- Windows Performance Toolkit for WPR/WPA tracing.
- WinDbg and Process Explorer for optional debugging and inspection.

Both programs use the Windows SDK and C/C++ runtime, with no third-party runtime or library required. Each source file has its own `main` function and must be compiled separately.

The examples intentionally skip systems with more than one active processor group. This avoids silently misinterpreting processor-group-relative affinity masks.

## 1. Equal configured priority on one logical processor

**Source:** `equal_priority.cpp`

### Objective

Observe two independent, finite CPU-bound workers competing for one eligible logical processor.

Equal configured priorities do not imply equal work counts, equal elapsed times, equal CPU accounting, or a precisely measurable time quantum. Use a scheduler trace to distinguish Running, Ready, and Waiting intervals.

### Build and run

```bat
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc equal_priority.cpp /Fe:equal_priority.exe /link /DEBUG
equal_priority.exe
equal_priority.exe --auto
```

Keep the generated PDB beside the executable for source and stack inspection.

### Execution and expected evidence

1. The program prints the process ID, thread IDs in decimal and hexadecimal, thread handles, the process-eligible affinity mask, and the selected logical processor.
2. It verifies `NORMAL_PRIORITY_CLASS` and configures both workers with `THREAD_PRIORITY_NORMAL`.
3. Both workers wait on a common manual-reset start event. The interactive checkpoint advances on Enter or after two seconds; redirected input advances immediately. `--auto` skips console input.
4. Main releases the workers. Each performs bounded CPU work for approximately six seconds, or one second in automatic mode, measured using its own elapsed wall time after leaving the event wait.
5. Main joins both workers, collects cycle-count accounting while the handles remain valid, prints independent iteration counts, elapsed times, checksums, and exit codes, and closes the handles.

The program selects the lowest set bit in the intersection of the process and system affinity masks. It does not assume that CPU 0 is available. Only worker affinity is changed; the main thread retains its inherited affinity and may execute on the same processor or another permitted processor.

The workers use `_beginthreadex` because they use C++ runtime facilities. Main and both workers receive thread descriptions.

**Expected duration:** approximately eight seconds or less interactively, and approximately one second in automatic mode. A shared 18-second deadline bounds waits and CPU work. Deadline checks occur between finite computation batches.

These limits are safety budgets, not real-time guarantees. Starvation, debugger stops, VM pauses, and scheduling delays can affect completion. No arbitrary-thread suspension or priority-class escalation is used.

### Process Explorer

Prepare symbols before launching the program. Configure Microsoft's public symbol server and the local PDB directory, then open the printed PID in Process Explorer.

Under **Properties → Threads**, identify `equal-cpu-a` and `equal-cpu-b` using their printed TIDs. Inspect CPU usage, Cycles Delta, priority, and Stack where available.

Compare the initial gate phase with the computation phase. Both workers can accumulate CPU usage during the same GUI refresh interval even though they cannot execute instructions simultaneously on the same logical processor. A GUI refresh aggregates many scheduling events; main, unrelated threads, interrupts, DPCs, and other processes can also consume processor time.

Do not suspend threads or change affinity or priority during the experiment.

### WinDbg

Launch the executable in x64 WinDbg and configure symbols:

```text
.symfix C:\Symbols
.sympath+ C:\labs\05_Priority_Preemption_And_Quantum
.reload /f
bu equal_priority!CpuWorker
g
```

The entry breakpoint occurs before the event wait. Once the program prints the worker identities, interrupt briefly during computation and inspect a worker:

```text
~
~~[HEX_TID]s
k
!teb
!runaway 7
!handle PRINTED_THREAD_HANDLE f
g
```

Replace `HEX_TID` with the printed OS TID in hexadecimal, without `0x`, and `PRINTED_THREAD_HANDLE` with the actual handle value.

`~~[HEX_TID]s` selects a thread by OS TID. By contrast, `~2s` selects debugger thread index 2; it does not select OS TID 2.

Clear the entry breakpoint with `bc *` after using it. `!runaway` reports cumulative CPU accounting, not a quantum meter. Debugger stops perturb scheduling and consume the run's time budget. The ordinary user-mode debugger does not expose the complete kernel scheduling history.

### WPR/WPA analysis

Install Windows Performance Toolkit. In an elevated console, check for an existing recording before starting one. Do not interrupt another user's or application's recording.

```bat
wpr -status
wpr -start GeneralProfile -filemode
equal_priority.exe
wpr -stop equal-priority.etl
wpa equal-priority.etl
```

Ensure the recording profile includes context-switch and ready-thread events. If CPU Usage (Precise) or the required scheduling information is missing, recapture with an appropriate WPR profile.

In WPA:

1. Open **CPU Usage (Precise)**.
2. Filter by the printed process ID and both worker TIDs.
3. Group by CPU, process, and thread.
4. Zoom into the common work interval.
5. Inspect ready time, wait time, running duration, switch identities and reasons, and stacks where collected.

Look for both workers alternating on the selected logical processor. Distinguish switches caused by blocking from those consistent with preemption. Do not label every running interval as one full quantum: higher-priority work, interrupts, boosts, and blocking can interrupt or end an interval. A context-switch trace alone does not specify the scheduler's quantum policy.

### PowerShell snapshot

For a coarser snapshot from another PowerShell console:

```powershell
$target = Get-Process -Id 1234
$target | Select-Object Id, ProcessName, PriorityClass, ProcessorAffinity
$target.Threads | Select-Object Id, ThreadState, BasePriority, CurrentPriority, TotalProcessorTime
```

Replace `1234` with the printed process ID. Snapshots can race thread exit and cannot replace an ETW scheduling trace.

## 2. Waking an above-normal-priority worker

**Source:** `higher_priority_preemption.cpp`

### Objective

Observe scheduling when an above-normal-priority worker becomes runnable while a normal-priority worker is computing on the same eligible logical processor.

The program emits correlatable markers, but it does not guarantee a context switch within a fixed number of microseconds or any particular fairness ratio. A wake request makes a worker eligible to run; the scheduler trace establishes when it actually runs.

### Build and run

```bat
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc higher_priority_preemption.cpp /Fe:higher_priority_preemption.exe /link /DEBUG
higher_priority_preemption.exe
higher_priority_preemption.exe --auto
```

Keep the matching PDB beside the executable for source and stack inspection.

### Execution and expected evidence

1. The program prints the process ID and all thread IDs in decimal and hexadecimal. Main verifies that the process uses `NORMAL_PRIORITY_CLASS`.
2. The normal worker uses `THREAD_PRIORITY_NORMAL`; the second worker uses `THREAD_PRIORITY_ABOVE_NORMAL`. Neither worker uses HIGH or REALTIME priority.
3. The program selects the lowest eligible logical processor from the process/system affinity-mask intersection. Both workers use that same processor, while main retains its inherited affinity.
4. Initially, both work gates are closed. The high-priority worker's readiness event marks its approach to the wake wait; only a stack or trace can establish the exact instant it enters the kernel wait.
5. After the initial checkpoint, main releases the normal worker. It signals its actual entry and computes for approximately six seconds, or one second in automatic mode.
6. In interactive mode, main provides a one-second inspection checkpoint before waking the above-normal worker. In automatic mode, it uses a bounded 250 ms observation interval without reading console input.
7. Main signals the high-priority worker's gate. That worker computes for approximately 500 ms of elapsed time and returns.
8. Main joins both workers and prints independent work counts, cycle accounting, start/end QPC timestamps, elapsed time, and results.

The main thread is named `preemption-main`; the worker names are `preemption-normal` and `preemption-above`.

The wake marker contains a QPC timestamp, QPC frequency, system uptime, PID, and TIDs. The timestamp is sampled before `SetEvent`, and console output introduces delay. It is a correlation anchor, not the exact kernel-ready instant or an injected ETW event.

Use the printed QPC frequency to calculate timestamp deltas. Do not treat WPA's relative timeline as an absolute QPC timestamp without aligning the clocks.

**Expected duration:** approximately eight seconds interactively, including the initial checkpoint, and approximately one second in automatic mode. Shared waits and work use an 18-second safety budget. Scheduling delays, debugger stops, or VM pauses can defeat a wall-clock budget. Work proceeds in finite batches with stop and deadline checks.

### Process Explorer

Open the printed PID in Process Explorer and select **Properties → Threads**. Identify `preemption-normal` and `preemption-above` by their printed decimal TIDs.

Inspect priority, CPU and cycle deltas, and stacks before and after the wake. GUI samples can miss the entire short high-priority burst, so repeat the experiment or use an uninterrupted trace.

Do not suspend threads or change priority through the GUI.

### WinDbg

Launch the executable in x64 WinDbg and set an entry breakpoint:

```text
.symfix C:\Symbols
.sympath+ C:\labs\05_Priority_Preemption_And_Quantum
.reload /f
bu higher_priority_preemption!CpuWorker
g
```

After the thread identities are printed, interrupt briefly and inspect:

```text
~
~~[HEX_TID]s
kn
!teb
!runaway 7
!handle PRINTED_THREAD_HANDLE f
bc *
g
```

Substitute the actual printed values. `~~[HEX_TID]s` uses hexadecimal OS TID digits without `0x`; `~2s` selects debugger index 2. A thread handle is neither a TID nor a debugger index.

`!runaway` provides cumulative CPU accounting, not dispatch latency. Debugger stops perturb priority observations and consume the elapsed-time budget. Capture timing evidence in a separate run without breakpoints.

### WPR/WPA analysis

Prepare Windows Performance Toolkit and an elevated recording console. Check the recording status and avoid disturbing an existing session:

```bat
wpr -status
wpr -start GeneralProfile -filemode
higher_priority_preemption.exe
wpr -stop "%TEMP%\higher-priority.etl"
```

In WPA, use **CPU Usage (Precise)** and filter by the printed PID, selected CPU, and both worker TIDs. Verify that the recording includes context-switch and ready-thread events; recapture if those events are absent.

Locate:

1. The normal worker's running interval.
2. The above-normal worker becoming ready.
3. Its first switch-in and eventual switch-out.
4. The normal worker's subsequent execution.

Inspect ready duration, switch-out state and reason, and switch-in priority where available. Compare the recorded ready events with the printed wake marker, accounting for the fact that the marker precedes `SetEvent`.

A normal worker displaced while still runnable is consistent with preemption. However, the higher-priority worker may become ready while main, another thread, or an interrupt already occupies the processor. A direct low-to-high switch at the printed timestamp is not required.

Ready duration is more informative than expecting one particular sequence of events. Toolkit versions may expose different columns.

### PowerShell snapshot

For a coarse second-console snapshot:

```powershell
$targetPid = 1234
(Get-Process -Id $targetPid).Threads |
    Select-Object Id, ThreadState, BasePriority, CurrentPriority, TotalProcessorTime
```

Replace `1234` with the printed process ID. Do not assign to PowerShell's read-only `$PID` variable. A snapshot can miss the short burst and can race process or thread exit.

## Shared interpretation and limitations

### Priority, readiness, and execution

- `GetThreadPriority` reports a configured relative priority, not necessarily the thread's current dynamic kernel priority.
- Dynamic boosts, foreground scheduling policy, wait-related boosts, background load, interrupts, power management, and processor topology can change the observed schedule.
- Neither program disables system boost policy, modifies registry settings, or escalates to real-time priority.
- Affinity limits where a thread is eligible to run; it does not reserve a processor or guarantee uninterrupted execution.
- A Ready thread is eligible to run but is not currently executing. A Waiting thread is blocked until its wait condition is satisfied.
- CPU-time and cycle-accounting values are not substitutes for a precise scheduler trace. Do not convert cycle counts to seconds using an assumed fixed CPU frequency.
- Main and unrelated threads, DPCs, interrupts, and other processes may affect observations.

### Timing and work counters

Both experiments use bounded computation and finite batches, with deadline checks between batches. A worker can cross a deadline while finishing a batch, and scheduling delays can prevent it from completing useful work in the expected interval.

Iteration counts and checksums make each worker's computation observable; they are not cryptographic values or fixed golden results. Unequal work counts do not by themselves prove unfair scheduling.

The measured wall interval includes scheduling delays and experiment overhead. CPU accounting reflects charged execution and has its own measurement semantics. Neither is a direct measurement of a scheduler quantum.

### Scope and safety

These experiments are intended for controlled analysis of processes you own or are authorized to inspect. They do not perform cross-process manipulation, arbitrary-thread suspension, forced thread termination, or registry modification.

Processor-group checks deliberately reject multi-group configurations. External affinity restrictions, job constraints, CPU sets, hot-plug events, virtualization, and debugger or tracing overhead can affect behavior.

Process Monitor may capture thread lifecycle events, but it does not provide complete scheduler-state history or establish every thread's origin or provenance. Thread start addresses and stacks require context; these experiments do not make malware-attribution claims.

A Linux cross-compilation check is not equivalent to validating runtime behavior on Windows.

## Learner questions

### Equal-priority contention

- Why can a worker be Ready while accumulating no additional CPU time?
- Why can equal configured priorities yield unequal work counts?
- Which trace evidence distinguishes blocking from loss of the processor?
- Why is a one-second wall interval not one second of per-thread CPU execution?
- What evidence is needed before calling a running interval a full quantum?

### Higher-priority wake-up

- What distinguishes the wake request, Ready state, and first Running interval?
- Why can the normal worker continue briefly after the printed wake marker?
- Why can configured priority differ from sampled current priority?
- Which observations distinguish a blocking switch from preemption?

## References

- Microsoft documentation: [Scheduling Priorities](https://learn.microsoft.com/en-us/windows/win32/procthread/scheduling-priorities)
- [GetProcessAffinityMask](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getprocessaffinitymask)
- [SetThreadAffinityMask](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setthreadaffinitymask)
- [GetThreadPriority](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreadpriority)
- [SetThreadPriority](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadpriority)
- [QueryThreadCycleTime](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime)
- [Windows Performance Recorder](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/windows-performance-recorder)
- [Windows Performance Analyzer](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/windows-performance-analyzer)

