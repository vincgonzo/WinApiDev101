# Windows Thread Scheduling: One CPU vs. Two CPUs

This lab compares two CPU-bound worker threads running on one or two eligible logical processors.

- `single_cpu.cpp` — both workers share one logical processor.
- `two_cpus.cpp` — each worker is assigned a different logical processor.

The objective is to observe scheduling, CPU consumption, and completed work, then distinguish counter measurements from evidence of actual execution overlap.

This is a scheduling experiment, not a benchmark with a fixed speedup target.

## Requirements

- Windows 10 version 1607 or later, or Windows 11.
- x64 Visual Studio C++ build tools and Windows SDK.
- Optional: Process Explorer, WinDbg, Windows Performance Toolkit (WPR/WPA).

Both programs are standalone console applications using the Windows SDK and C/C++ runtime. No third-party libraries or elevation are required.

Both programs deliberately support only systems with one active processor group. They use `_beginthreadex` for CRT-using workers and resolve `SetThreadDescription` dynamically for Windows 10 version 1607 compatibility.

## Build

Run these commands from an x64 Native Tools Command Prompt in the directory containing the source files.

**One logical processor**

```bat
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc single_cpu.cpp /Fe:single_cpu.exe /link /DEBUG
single_cpu.exe
single_cpu.exe --auto
```

**Two logical processors**

```bat
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc two_cpus.cpp /Fe:two_cpus.exe /link /DEBUG
two_cpus.exe
two_cpus.exe --auto
```

Normal mode provides a preparation checkpoint of up to 30 seconds, followed by a shared six-second work window. Press Enter to continue.

The `--auto` option skips the checkpoint and requests a one-second work window. The intended total runtime is under 20 seconds. Non-console input also skips the checkpoint.

Exit codes:

- `0` — structural, placement, and cleanup checks passed.
- `1` — usage error, API failure, timeout, or failed invariant.
- `77` — unsupported processor-group or affinity configuration. No workers are created on this path.

## Experiment 1: Two Workers, One Logical Processor

**Source:** `single_cpu.cpp`

### Objective

Make two CPU-bound workers eligible to execute on the same logical processor, then observe how Windows schedules them.

### Execution

The program prints the PID and main TID in decimal and hexadecimal, checks that the system has exactly one active processor group, and reads `GetProcessAffinityMask`.

It selects the first eligible processor from the intersection of the process and system affinity masks. CPU 0 is not assumed to be available.

Both workers, `single-cpu-A` and `single-cpu-B`, are assigned the same single-bit affinity mask before the common start event is released. Their TIDs, selected processor, and previous affinity masks are printed.

The coordinator, `single-cpu-coordinator`, retains its original affinity. Worker priorities and process priority remain normal.

Prepared events acknowledge worker entry. A common start gate coordinates the experiment, but it does not guarantee simultaneous execution.

Each worker then:

1. Checks its actual group affinity.
2. Records its first and last processor snapshots.
3. Signals its approach to the arithmetic routine.
4. Runs `BurnCpuUntil` using worker-local unsigned 64-bit arithmetic.
5. Checks a shared absolute deadline between batches of 4096 steps.

The hot loop uses no shared counter, spinlock, or console-output loop. Iteration counts and checksums are reported so that the work remains observable.

Main joins both workers before reading their results. It checks exit codes, affinity, processor snapshots, batch alignment, monotonic counters, and unchanged coordinator affinity.

The program reports wall runtime, user and kernel CPU time, cycle deltas, completed iterations, and checksums.

The requested work window is six seconds in normal mode and one second in automatic mode. A worker dispatched late receives less time. If it completes no batch, the program reports that the sample is uninformative rather than requiring a minimum throughput.

### Process Explorer

Start Process Explorer before launching the program. At the preparation checkpoint, locate the printed PID and open **Properties → Threads**.

Match both worker TIDs. Display the available CPU, CPU Time, and Cycles Delta columns, then press Enter in the program console.

During the work interval, inspect both worker stacks. With matching PDBs and Microsoft symbols configured, expect `AffinityWorker`, `BurnCpuUntil`, or a clock-check routine.

Both workers contend for one eligible logical processor. Their CPU shares need not be equal. Other processes, interrupts, and the coordinator also affect scheduling.

Do not change process affinity through the GUI during the experiment: doing so changes the test and may constrain the coordinator as well.

### WinDbg

Launch under x64 WinDbg or attach during the preparation checkpoint.

```text
.symfix
.sympath+ C:\path\to\this\lab
.reload /f
~
~~[HEX_TID]s
k
!teb
!runaway 7
x single_cpu!*BurnCpuUntil*
bu single_cpu!BurnCpuUntil
g
```

Replace `HEX_TID` with worker A's printed hexadecimal TID, without the `0x` prefix. `~~[HEX_TID]s` selects a thread by OS TID; `~1s` uses a debugger thread index.

At the arithmetic-entry breakpoint:

```text
k
dv /t /v
u single_cpu!BurnCpuUntil
bl
```

Clear the specific lab breakpoint using its number from `bl`, or use `bc *` only in a dedicated lab session where removing all breakpoints is intentional. Use
