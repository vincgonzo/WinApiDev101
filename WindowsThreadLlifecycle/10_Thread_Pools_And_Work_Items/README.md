# Dedicated Threads vs. Windows Thread Pool Work Items

**Companions:** `dedicated_threads.cpp` and `thread_pool_callbacks.cpp`  
**Navigation:** Training overview · Tool setup

## Learning objectives

Understand the difference between an application task and the operating-system thread that executes it.

This exercise implements six independent tasks in two ways:

- **Dedicated threads:** Each task gets its own explicitly created OS thread.
- **Private thread pool:** Six work items are submitted to a Windows thread pool configured to use at most two workers concurrently.

Both implementations perform a finite checksum calculation and a 150 ms sleep. This makes the execution easier to observe without turning the exercise into a performance benchmark.

The key distinction is that **a work item is not a thread**. Six tasks do not require six distinct OS threads, and a thread can execute multiple tasks over its lifetime.

## Environment and build

Requirements: Windows 10 version 1607 or later, or Windows 11, with the x64 Windows SDK and C/C++ runtime.

Open an **x64 Native Tools Command Prompt** in the directory containing the source files.

### Build and run the dedicated-thread implementation

```bat
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc dedicated_threads.cpp /Fe:dedicated_threads.exe /link /DEBUG
dedicated_threads.exe
dedicated_threads.exe --auto
```

### Build and run the private thread-pool implementation

```bat
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc thread_pool_callbacks.cpp /Fe:thread_pool_callbacks.exe /link /DEBUG
thread_pool_callbacks.exe
thread_pool_callbacks.exe --auto
```

For Visual Studio, create an x64 Console App, disable precompiled headers, replace the generated source with the corresponding file, and select C++17, `/Od`, `/Zi`, and linker `/DEBUG`. Keep the Console subsystem and the matching executable/PDB.

Neither program requires third-party packages or shared training headers. Both resolve thread-naming APIs dynamically from `KernelBase.dll` to support the stated Windows compatibility target.

### Execution modes

| Mode | Behaviour |
|---|---|
| Normal | Provides interactive checkpoints for inspection with Process Explorer or WinDbg. |
| `--auto` | Skips the interactive checkpoints and uses shorter waits where applicable. |

The automatic runs are designed to finish in roughly one second, within the exercise's 20-second budget. These are not hard real-time guarantees: a debugger pause, system load, or unusual scheduler delay can affect execution.

Both programs return process exit code `0` on success and `1` on an API or invariant failure. A worker's own return value is separate from the process exit code.

## Experiment 1 — Six dedicated threads

**Source:** `dedicated_threads.cpp`

### Design

The program intentionally creates one dedicated thread for each of six independent tasks.

Main creates all six threads and their entry events before joining any worker. Every worker signals its own entry event and then waits on a common, initially unsignaled start event.

Main waits for all six entry events before offering the inspection checkpoint. This avoids a misleading create/join/create sequence in which Windows can recycle a thread ID (TID) and make multiple short-lived threads appear to be one reused worker.

### Expected execution

1. Main prints its process ID (PID) and thread ID (TID), in decimal and hexadecimal.
2. Main creates six workers named `dedicated-task-1` through `dedicated-task-6`.
3. Each worker signals its entry event and waits at the common gate.
4. Main confirms that six distinct worker TIDs have arrived and checks that the workers are still alive.
5. At the checkpoint, inspect the threads and their stacks, then press Enter.
6. Main opens the common gate. Each worker calculates a finite integer checksum and performs one 150 ms non-alertable sleep.
7. Main joins all six workers and prints six completed task records containing the TID, checksum, elapsed work time, and exit code.

All worker contexts and event handles remain valid until the joins finish. Main reads the result records only after joining, so it does not poll worker-written data without synchronization.

`CreateThread` is intentional here: the worker code uses Win32 APIs and integer operations but does not call the C runtime. Console I/O stays in main.

### Inspect with Process Explorer

At the gate checkpoint:

1. Open the process properties for the printed PID.
2. Select **Threads**.
3. Find `dedicated-task-1` through `dedicated-task-6` and record their TIDs.
4. Inspect a worker's stack. `DedicatedTask` should appear beneath event-wait frames, although Windows startup frames may also be present.

The process can contain unrelated runtime or tool-created threads, so its total thread count does not have to equal seven. At the checkpoint, the six application workers are alive but intentionally waiting; six live threads do not imply six threads running on CPUs simultaneously.

An optional PowerShell snapshot:

```powershell
(Get-Process -Id DECIMAL_PID).Threads |
    Select-Object Id, ThreadState
```

Replace `DECIMAL_PID` with the printed process ID. This snapshot reports OS thread IDs and states; it does not identify every application task or reconstruct scheduler history.

### Inspect with WinDbg

Launch the normal executable under x64 WinDbg with its matching PDB:

```text
.symfix
.sympath+ C:\training\10_Thread_Pools_And_Work_Items
.reload
x dedicated_threads!*DedicatedTask*
g
```

When the console reaches the gate checkpoint, break into the debugger and inspect the threads:

```text
~
~* k
~~[HEX_TID]s
k
r rip
ln @rip
```

Replace `HEX_TID` with a printed worker TID in hexadecimal, without `0x`. The `~` command displays debugger thread indexes; these indexes are not OS TIDs. Use `~~[HEX_TID]s` to select a thread by its OS TID.

Continue with `g` before pressing Enter in the console.

For another run, set a breakpoint on the worker function before continuing:

```text
bu dedicated_threads!DedicatedTask
g
```

The breakpoint can fire six times, once for each genuine function entry. Because the checksum work is short, a later snapshot may miss it. Debugger breaks can also exhaust the gate timeout; restart the experiment if necessary.

### Important limitations

- Dedicated threads make thread ownership and shutdown explicit, but one thread per small task can waste scheduling and stack resources in real applications.
- Dedicated threads are still appropriate for some long-lived state, thread affinity, or message-loop designs.
- Process Monitor (Procmon) can corroborate thread creation and exit if tracing begins early, but it does not identify each application task or provide complete scheduler history.
- Windows Performance Recorder/Analyzer (WPR/WPA) CPU samples may capture the checksum code, but a missing sample does not prove that the code did not execute.
- User-mode stack inspection is not direct access to kernel `ETHREAD` or `KTHREAD` objects.

## Experiment 2 — Six work items on a private Windows thread pool

**Source:** `thread_pool_callbacks.cpp`

### Design

This program submits six independent work items to a private Windows thread pool configured with:

- Minimum worker count: `1`
- Maximum worker count: `2`
- Total submitted work items: `6`

Each callback records the TID on which it executes, calculates a finite checksum, sleeps for 150 ms, and returns.

No callback waits for another task in this pool. This independence is important: a constrained pool can deadlock if all its workers block while waiting for work items that are queued behind them.

Main performs all C runtime console I/O. The callbacks use Win32 operations and integer arithmetic. Each result record has one writer, and main reads the records only after callback completion. Shared concurrency counters use `Interlocked` operations rather than relying on `volatile` alone for synchronization.

### Expected execution

1. Main prints its PID and TID, in decimal and hexadecimal.
2. Main creates the private pool, callback environment, and six work objects.
3. The first checkpoint occurs before submission, allowing a debugger to install a callback breakpoint.
4. Main submits all six work items before waiting for any of them.
5. Main calls `WaitForThreadpoolWorkCallbacks(item, FALSE)` for each work object.
6. Once all callbacks have completed, main prints the six task records, observed TIDs, repeated TID assignments, and peak concurrent callback count.
7. At the final checkpoint, the work objects, environment, and pool still exist. After Enter, main closes every work object, destroys the callback environment, and closes the pool.

Callbacks name their executing threads `private-pool-worker`; main is named `pool-main`.

Six records commonly contain only one or two distinct TIDs. However, the program does not require a particular callback order or require both workers to execute work. It verifies that all tasks complete and that no more than two callback bodies execute concurrently. It reports TID reuse evidence without failing just because a particular distribution was not observed.

The pool's maximum of two applies to concurrent workers in that pool, not to the total number of threads in the process or the lifetime number of distinct TIDs. Windows can replace workers over time.

### Understand callback completion and cancellation

The call:

```cpp
WaitForThreadpoolWorkCallbacks(item, FALSE);
```

waits for the callbacks associated with the work object to complete without cancelling pending callbacks.

Passing `TRUE` would cancel callbacks that have not started. It would **not forcibly interrupt callbacks that are already running**.

These waits concern callbacks associated with work objects, not joins on individually created OS threads. The example does not use callback disassociation.

The callback-wait API has no timeout parameter. Although the workload is finite and normally finishes quickly, completion still assumes ordinary scheduler progress and no debugger stop.

### Inspect with Process Explorer

At the final checkpoint:

1. Open the process properties for the printed PID.
2. Select **Threads**.
3. Look for the recorded TIDs and `private-pool-worker` descriptions.
4. Inspect the thread stacks where available.

Pool workers may be idle or may already have retired. Do not assume exactly two pool threads must remain visible. A worker's start address often points to a generic Windows thread-pool startup routine rather than `PoolWorkCallback`; the exact symbols and frames depend on the Windows version.

An idle stack snapshot cannot show a callback that has already returned. The callback breakpoint below is a better way to capture the actual callback execution stack.

Repeated TIDs across the six records are evidence that the same TIDs were assigned to multiple tasks in this run. If you need to distinguish reuse from TID recycling across separate thread lifetimes, capture thread creation and exit events from process startup.

### Inspect with WinDbg: catch an actual callback

Launch the normal executable under x64 WinDbg:

```text
.symfix
.sympath+ C:\training\10_Thread_Pools_And_Work_Items
.reload
x thread_pool_callbacks!*PoolWorkCallback*
bu thread_pool_callbacks!PoolWorkCallback
g
```

Press Enter at the first console checkpoint. At each callback breakpoint:

```text
~
~.
k
r rip
ln @rip
dv /t
g
```

Record the current OS TID at successive breakpoint hits. The callback should execute on a pool worker beneath pool-dispatch frames; a new thread is not required for each hit.

When finished with the breakpoints, use:

```text
bl
bc *
g
```

At the final checkpoint, break again and select a still-live recorded worker:

```text
~~[HEX_TID]s
k
lm
!address @rip
```

Replace `HEX_TID` with its printed hexadecimal TID, without `0x`. A retired worker cannot be selected, and debugger indexes are not OS TIDs. These commands inspect user-mode threads and stacks; they do not directly inspect kernel `ETHREAD` objects.

### Optional tracing and limitations

- Procmon thread creation and exit events can corroborate worker lifetimes, but they are not a work-item log.
- WPR/WPA CPU Usage sampling may capture callback frames if samples land during execution. This small workload is not guaranteed to produce many samples.
- A generic worker start address establishes execution provenance, not the semantics of every task dispatched by that worker.
- A configured pool maximum is a concurrency limit for that pool, not a lifetime limit on distinct worker TIDs.

## Direct comparison

| Property | Dedicated threads | Private thread pool |
|---|---|---|
| Application tasks | 6 | 6 |
| Worker allocation | One explicitly created thread per task | Pool-managed workers shared by tasks |
| Configured concurrency | Six workers can reach the gate; scheduling determines actual execution | At most two callback bodies execute concurrently |
| Thread ownership | Main creates and joins each thread | Windows manages pool worker lifetimes |
| Completion primitive | Wait/join on each thread handle | `WaitForThreadpoolWorkCallbacks` on each work object |
| Typical TID pattern | Six distinct worker TIDs at the gate | Task records commonly share one or two TIDs |
| Stack inspection | `DedicatedTask` under wait or execution frames | `PoolWorkCallback` under pool-dispatch frames |
| Resource implications | More per-thread scheduling and stack resources | Can reuse workers across many short tasks |
| Best fit | Long-lived state, affinity, or dedicated message loops when justified | Many independent, relatively short work items |

The two programs use similar task bodies so the main difference is how execution is managed, rather than the amount of work performed. Neither program is intended to benchmark throughput or prove deterministic scheduling behaviour.

## Learner questions

1. Why does holding all six dedicated workers at one gate rule out TID recycling between those six live threads at that checkpoint?
2. Why is it safe for main to read the dedicated-thread result records only after joining the workers?
3. Could six live dedicated threads execute on a single logical processor? Why?
4. Why can six completed pool work items contain repeated TIDs?
5. What does `FALSE` preserve in `WaitForThreadpoolWorkCallbacks`?
6. Why could a callback that waits for another work item exhaust a pool configured with two workers?
7. Why is an idle stack snapshot different from one captured at the callback breakpoint?
8. What evidence should differ between the two implementations, and what conclusions would be unjustified from a single snapshot?

## Official references

- `CreateThread`
- `WaitForMultipleObjects`
- `Using Event Objects`
- `Thread Handles and Identifiers`
- WinDbg thread syntax
- Windows thread pools
- `CreateThreadpoolWork`
- `SetThreadpoolThreadMaximum`
- `SetThreadpoolThreadMinimum`
- `WaitForThreadpoolWorkCallbacks`
- `CloseThreadpool`

## Key takeaway

**A task describes work; a thread describes an execution context.** Dedicated threads explicitly associate one OS thread with each task in this experiment. The private thread pool allows six tasks to share a constrained set of workers. Observe task results, TIDs, and callback stacks together to understand the distinction—without treating any one snapshot as a complete record of scheduler history.
