# Windows Mutexes: Contention, Ownership, and Deadlock Recovery

This lab explores two Windows mutex synchronization scenarios:

- **Mutex contention:** one thread owns a mutex while another attempts to acquire it. The owner releases it cooperatively, allowing the contender to proceed.
- **Circular mutex wait:** two threads acquire two mutexes in opposite orders, creating a circular dependency that is broken by finite timeouts.

The objective is to distinguish mutex ownership, attempted acquisition, actual waiting, and deadlock recovery. Thread announcements and source-level logic establish the intended order, while stacks and scheduler traces provide evidence of the runtime state.

See [`TOOLS.md`](TOOLS.md) and the training overview for shared setup instructions.

## Requirements

- Windows 10 version 1607 or later, or Windows 11, x64.
- An x64 Native Tools Command Prompt with the Windows SDK.
- Windows Performance Toolkit for WPR/WPA tracing.
- Process Explorer and, optionally, x64 WinDbg.

Both examples use the Windows SDK and C/C++ runtime without third-party libraries. Compile each source file separately. Keep the executable, source, and matching PDB together for debugging.

## 1. Mutex contention with an explicit owner

**Source:** `mutex_contention.cpp`

### Objective

Distinguish a runnable CPU thread from a thread waiting for a mutex owned by another thread. Establish the ownership order using synchronization events and a positive-timeout acquisition probe, then release the mutex cooperatively.

### Build and run

```bat
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc mutex_contention.cpp /Fe:mutex_contention.exe /link /DEBUG
mutex_contention.exe
mutex_contention.exe --auto
```

`/Od /Zi` and `/link /DEBUG` preserve useful source locations, local variables, and stack information.

### Execution and expected results

1. Main prints the PID, thread IDs in decimal and hexadecimal, thread handles, mutex handle, and handshake-event handles.
2. The owner acquires the mutex, writes protected value `1`, and signals `owner-owns`.
3. Main starts the contender. The contender announces readiness and attempts to acquire the mutex with a 100 ms timeout. The owner has not released it, so this attempt must time out.
4. The contender signals `contention-observed` and approaches a second bounded acquisition wait, which also listens for an abort event.
5. Main exposes the inspection checkpoint only after the ownership, readiness, and contention handshakes. During this phase, the owner waits for main's release or abort signal while retaining the mutex. The contender is approaching or inside its mutex wait.
6. Enter or the four-second checkpoint deadline makes main signal `release-owner`. The owner writes protected value `2` and calls `ReleaseMutex`.
7. The contender acquires the mutex, verifies value `2`, writes value `3`, and releases it.
8. Main joins both workers, checks results and errors, and closes the handles.

The protected value is accessed only while the accessing thread owns the same mutex. Main never reads or modifies it directly. Result flags are atomic, and worker state and synchronization objects have process lifetime to avoid dangling pointers on error paths.

An abandoned mutex is treated as acquired ownership, but the exercise fails because the protected state may be inconsistent. The mutex is released rather than silently treating abandonment as successful recovery.

**Expected duration:** approximately 4.1 seconds interactively without pressing Enter; automatic mode normally takes around 100 ms. Console input is bounded, and redirected or non-console input advances immediately. Operations share an 18-second safety deadline.

This is **contention, not deadlock**: main has a bounded path to release the owner. Failure requests cooperative abort; synchronization handles are closed only after the workers have joined.

### Expected evidence

During the inspection phase:

- `mutex-owner` should be waiting for main's release or abort event while retaining ownership.
- `mutex-contender` should be in or approaching a mutex-related wait.
- The initial 100 ms timeout confirms that the mutex was unavailable during the first probe.
- The successful second acquisition demonstrates that the owner's release allowed the contender to proceed.

A readiness event establishes program progression, not the exact instant the contender entered a kernel wait. Use a stack or scheduler trace to inspect the actual state.

## 2. Circular mutex wait with automatic recovery

**Source:** `bounded_deadlock.cpp`

### Objective

Construct a circular dependency by acquiring two mutexes in opposite orders. Observe the cross-waits, then recover through finite timeouts without forced termination, arbitrary suspension, external repair, or user input.

The key distinction is between observing that both workers own their first mutexes and observing that both are actually blocked while requesting the other's mutex.

### Build and run

```bat
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc bounded_deadlock.cpp /Fe:bounded_deadlock.exe /link /DEBUG
bounded_deadlock.exe
bounded_deadlock.exe --auto
```

Both workers use `_beginthreadex` and atomic result fields. Preserve the matching PDB for source and stack inspection.

### Execution and expected results

1. Main prints the PID, main and worker TIDs in decimal and hexadecimal, thread and mutex handles, and each worker's acquisition order.
2. Worker A acquires mutex A; worker B acquires mutex B. Each signals its first-owned event while retaining ownership.
3. Main waits for both ownership events. Phase 1 explicitly occurs **before** the cross-wait cycle: the workers remain behind a gate.
4. Phase 2 releases the common cross-wait gate. Each worker announces its cross-acquisition phase and waits for the other worker's announcement before requesting the other's mutex. Both retain their first mutex.
5. Once both announcements have arrived, phase 3 provides a timed inspection checkpoint. At this point, both workers are expected to be approaching or inside their second mutex waits.
6. Each cross-acquisition uses a 15,000 ms timeout interactively or a 1,000 ms timeout in automatic mode, capped by the remaining 18-second run budget.
7. At least one cross-wait must time out. That worker releases its first mutex, breaking the circular dependency. The other worker may then acquire its second mutex and release both, or it may also time out.
8. Main joins both workers, checks results and errors, verifies that one or two expected timeouts occurred, and closes the handles.

The phase-3 checkpoint advances on Enter or after one second; it does not control lock release. Recovery continues independently while the checkpoint is open.

The announcements establish code progression, not the precise kernel wait state. An unusually delayed or debugged run may time out before both blocked workers can be captured simultaneously. Do not claim to have observed a simultaneous circular wait based on a phase label alone.

`WAIT_ABANDONED` is handled distinctly: it grants ownership but indicates that protected state may be inconsistent. The worker releases what it owns and fails the exercise rather than treating abandonment as the intended timeout recovery.

**Expected duration:** approximately 15 seconds interactively and around one second in automatic mode. Preparation and join waits share an 18-second budget. No CPU spin loop or unbounded input wait is used.

## Shared analysis tools

Prepare symbols before launch, as described in `TOOLS.md`. Both programs print process and thread identities to help correlate source, stacks, and traces.

### Process Explorer and wait-chain analysis

Open the printed PID in Process Explorer and select **Properties → Threads**.

For `mutex_contention.exe`, identify:

- `mutex-main`
- `mutex-owner`
- `mutex-contender`

For `bounded_deadlock.exe`, identify:

- `deadlock-main`
- `deadlock-a-then-b`
- `deadlock-b-then-a`

Inspect the worker stacks during each program's inspection phase. Low CPU usage is consistent with waiting but does not prove that a thread is blocked on a particular mutex.

Where supported, Task Manager's **Details → right-click process → Analyze wait chain** may reveal some dependencies. Coverage and permissions vary, especially when multiple-object waits and abort events are involved. An empty or incomplete wait-chain display is not proof that contention or a deadlock is absent.

Never use end-thread, end-process, suspend, or resume actions to recover these examples. Both are designed to recover or terminate their experiment through cooperative synchronization and finite timeouts.

### WinDbg

Launch the executable in x64 WinDbg and configure symbols. Use the matching command for the program being inspected.

For `mutex_contention.exe`:

```text
.symfix C:\Symbols
.sympath+ C:\labs\07_Lock_Contention_And_Deadlock
.reload /f
bu mutex_contention!OwnerMain
bu mutex_contention!ContenderMain
g
```

For `bounded_deadlock.exe`:

```text
.symfix C:\Symbols
.sympath+ C:\labs\07_Lock_Contention_And_Deadlock
.reload /f
bu bounded_deadlock!DeadlockWorker
g
```

These entry breakpoints occur before the relevant waits. Clear them and continue before interpreting the actual contention or circular dependency.

During the inspection phase, break briefly and inspect the actual worker identities and handles.

For the contention example:

```text
bc *
~
~~[OWNER_HEX_TID]s
kn
~~[CONTENDER_HEX_TID]s
kn
!handle PRINTED_MUTEX_HANDLE f
!handle PRINTED_RELEASE_EVENT_HANDLE f
g
```

For the circular-wait example:

```text
bc *
~
~~[WORKER_A_HEX_TID]s
kn
~~[WORKER_B_HEX_TID]s
kn
!handle MUTEX_A_HANDLE f
!handle MUTEX_B_HANDLE f
g
```

Replace placeholders with the actual printed values. `~~[HEX_TID]s` selects a thread by hexadecimal OS TID without the `0x` prefix. `~2s` selects debugger index 2 instead. A thread handle is neither a TID nor a debugger index, and handles are process-local.

`!handle actualhandle f` displays the object information available in that debugger and OS version. `!locks` commonly describes user-mode critical sections; it is not a general browser for arbitrary Windows mutex ownership.

For deeper wait-state evidence, kernel WinDbg in a dedicated debugging VM can inspect the process and its threads:

```text
!process 0 0 bounded_deadlock.exe
!process EPROCESS_ADDRESS 7
!thread ETHREAD_ADDRESS
```

Use actual kernel addresses returned by the debugger, not numeric TIDs, as `EPROCESS_ADDRESS` and `ETHREAD_ADDRESS`.

Debugger pauses affect timeout behavior. A mutex timeout may expire while the process is stopped, but the code that handles the timeout and releases the mutex cannot run until the process resumes. Long stops can exhaust the overall budget or prevent a simultaneous deadlock snapshot. Rerun for an uninterrupted recovery and ETW test; do not add a pre-release pause that falsely claims the circular wait already exists.

### WPR/WPA scheduler tracing

Install Windows Performance Toolkit. Use an elevated recording console, check the recording status, and avoid interfering with an existing WPR session.

For the contention example:

```bat
wpr -status
wpr -start GeneralProfile -filemode
mutex_contention.exe
wpr -stop "%TEMP%\mutex-contention.etl"
```

For the circular-wait example:

```bat
wpr -status
wpr -start GeneralProfile -filemode
bounded_deadlock.exe
wpr -stop "%TEMP%\bounded-deadlock.etl"
```

In WPA, open **CPU Usage (Precise)** and filter by the printed PID and worker TIDs. The recording profile must include context-switch and ready-thread events; recapture with an appropriate profile if the necessary scheduling information is missing.

For mutex contention, inspect:

- The contender's initial 100 ms timeout.
- Its subsequent wait interval.
- The owner's release.
- The contender's subsequent ready and running intervals.

For the circular-wait example, inspect:

- Both workers' cross-acquisition wait intervals.
- Switch-out stacks and wait intervals.
- The timeout that breaks the dependency.
- The subsequent release, readying, and running intervals.

Align the program's printed uptime timestamps with process and thread activity. Scheduler wait reasons are evidence about thread state, not a complete application-level lock graph. Combine the trace with the source-level acquisition order, mutex handles, and ownership handshakes.

### PowerShell snapshots

A coarse second-console snapshot can help identify waiting threads. Replace `1234` with the printed process ID.

```powershell
$targetPid = 1234
(Get-Process -Id $targetPid).Threads | ForEach-Object {
    $reason = if ($_.ThreadState -eq 'Wait') { $_.WaitReason } else { $null }
    [pscustomobject]@{
        TID        = $_.Id
        State      = $_.ThreadState
        WaitReason = $reason
    }
}
```

Do not assign to PowerShell's read-only `$PID` variable. Snapshots can race with thread recovery or exit and cannot establish a complete circular dependency.

Process Monitor can provide lifecycle context, but it does not show the complete scheduler/wait history or establish every thread's origin or provenance.

## Shared interpretation and limitations

### Ownership versus waiting

- A thread owning a mutex and a thread waiting to acquire it are different states.
- An event announcing an attempted acquisition does not prove the thread is already blocked in the kernel.
- A single blocked thread does not prove a deadlock. A circular deadlock requires evidence of a dependency cycle.
- In the contention example, the owner has an explicit cooperative release path, so the scenario is not a deadlock.
- In the circular-wait example, opposite acquisition order plus hold-and-wait creates the dependency. A timeout breaks the cycle by allowing a worker to release its first mutex.

### Timeout and abandoned-mutex behavior

Timeouts demonstrate one possible bounded recovery strategy; they are not a universal fix for inconsistent protected state or a substitute for consistent lock ordering in production code.

`WAIT_ABANDONED` is neither an ordinary timeout nor an ordinary successful acquisition. The caller receives ownership, but the previous owner exited without releasing the mutex, so protected data may be inconsistent. These examples release the acquired mutex and report failure rather than inventing a data-repair policy.

### Timing and runtime guarantees

Both examples use finite waits and bounded run budgets, but neither provides hard real-time guarantees. Debugger stops, VM pauses, starvation, and scheduling delays can exceed the intended wall-clock duration.

Worker state and synchronization objects have process lifetime, and handles are closed only after worker joins. Failure paths request cooperative abort rather than forcibly terminating workers.

Both examples use private unnamed mutexes and do not perform cross-process actions, arbitrary-thread suspension, forced termination, priority escalation, or registry changes.

A Linux cross-compilation check validates neither live Windows synchronization behavior nor scheduler-state observations.

## Learner questions

### Mutex contention

- Which handshake proves ownership, and which proves that contention occurred?
- Why may only the owner call `ReleaseMutex` in this example?
- How can the owner be waiting while still blocking the contender?
- Why must an abandoned-mutex result release ownership even though the exercise fails?

### Circular mutex wait

- Why is phase 1 not yet the circular second-mutex wait?
- Why is one timeout followed by another acquisition a successful recovery result?
- What evidence connects a blocked thread to the mutex owner?
- Why does `!locks` not establish ownership of arbitrary Windows mutex handles?
- What changes when a debugger stops the process for longer than the cross-wait timeout?

## References

- [Using Mutex Objects](https://learn.microsoft.com/en-us/windows/win32/sync/using-mutex-objects)
- [WaitForMultipleObjects](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects)
- [Wait Chain Traversal](https://learn.microsoft.com/en-us/windows/win32/debug/wait-chain-traversal)
- [WinDbg `!handle`](https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/-handle)
- [WinDbg `!thread`](https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/-thread)
- [Windows Performance Recorder](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/windows-performance-recorder)
- [Windows Performance Analyzer](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/windows-performance-analyzer)

