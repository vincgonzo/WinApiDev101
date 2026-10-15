# Blocking vs. Polling

This lab compares two ways of waiting for a Windows event using the Win32 API:

- `blocking_wait.cpp` — waits for an event using a finite, blocking `WaitForSingleObject` call.
- `polling_wait.cpp` — repeatedly calls `WaitForSingleObject` with a zero timeout.

Both programs use an initially unsignaled, unnamed, manual-reset event. They create separate event objects; they do not share an event across processes.

The objective is to understand the CPU cost of polling, the behavior of blocking waits, and the difference between CPU accounting and scheduler-state evidence.

## Requirements

- Windows 10 version 1607 or later, or Windows 11.
- x64 Visual Studio C++ build tools and Windows SDK.
- Optional: Process Explorer, WinDbg, Windows Performance Toolkit (WPR/WPA).

No third-party libraries or elevation are required. The worker uses `_beginthreadex` because it uses the C/C++ runtime. `SetThreadDescription` is resolved dynamically for Windows 10 version 1607 compatibility.

## Build

Run these commands from an x64 Native Tools Command Prompt in the directory containing the source files.

**Blocking wait**

```bat
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc blocking_wait.cpp /Fe:blocking_wait.exe /link /DEBUG
blocking_wait.exe
blocking_wait.exe --auto
```

**Polling wait**

```bat
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc polling_wait.cpp /Fe:polling_wait.exe /link /DEBUG
polling_wait.exe
polling_wait.exe --auto
```

Normal mode offers a preparation checkpoint of up to 30 seconds, followed by an observation interval of approximately five seconds. Press Enter to continue.

The `--auto` option skips the checkpoint and uses an observation interval of approximately one second. It is intended to complete in under 20 seconds. Non-console input also skips the prompt.

Unknown or extra arguments are rejected. Exit code `0` indicates success; exit code `1` indicates an error or failed check.

## Experiment 1: Blocking Wait

**Source:** `blocking_wait.cpp`

### Objective

Observe a worker that asks Windows to wait for an unsignaled event rather than repeatedly checking its state.

### Execution

The program prints the process ID (PID), main thread ID (TID), and worker TID in decimal and hexadecimal. The worker is named `blocking-event-wait`; the coordinator is named `blocking-coordinator`.

A preparation event confirms worker entry. The worker then waits on a separate start gate while the tools are prepared.

Main samples the worker's CPU time using `GetThreadTimes` and its cycle count using `QueryThreadCycleTime`. It then starts the observation interval and opens the start gate.

Immediately before calling `WaitForSingleObject` on the completion event, the worker signals an approach event. This handshake confirms that the worker reached the point immediately before the wait call; it does not prove that Windows has already placed the thread in the Waiting state.

Main leaves the completion event unsignaled and waits on the worker handle for the requested observation interval. The wait must time out. An early worker exit is treated as a failure.

After the observation interval, main signals the completion event, joins the worker, checks its recorded wait result and exit code, and collects the final counters.

The program reports:

- Worker user-mode CPU time.
- Worker kernel-mode CPU time.
- Total CPU-time delta.
- Thread cycle-count delta.
- Measured gate-to-join wall-clock time.

The measured interval includes synchronization, scheduling, and shutdown overhead. It is not a measurement of the kernel wait alone.

### Process Explorer

Start Process Explorer before launching the program. At the preparation prompt, locate the printed PID and open **Properties → Threads**.

Identify the worker using its printed TID. Where supported, display the CPU, CPU Time, and Cycles Delta columns.

Press Enter in the console to begin the observation interval. Select the worker and inspect its stack. With matching PDBs and Microsoft symbols configured, expect `BlockingEventWorker` and an event-wait path that may include `WaitForSingleObjectEx` or `NtWaitForSingleObject`.

Exact system frames vary by Windows and tool version. A stack captured at the preparation prompt may show the start-gate wait instead of the measured completion-event wait.

### WinDbg

Launch the program under x64 WinDbg or attach during the preparation checkpoint.

```text
.symfix
.sympath+ C:\path\to\this\lab
.reload /f
~
~~[HEX_TID]s
k
!teb
!runaway 7
x blocking_wait!*BlockingEventWorker*
g
```

Replace `HEX_TID` with the printed hexadecimal worker TID, without the `0x` prefix. `~~[HEX_TID]s` selects a thread by OS TID; `~1s` selects debugger thread index 1.

Allow the run to begin, then break briefly and inspect the worker again using `k` and `!runaway 7`. Resume promptly with `g`.

`!runaway 7` reports accumulated thread time; it does not identify every Ready interval. User-mode `!teb` does not establish a kernel scheduler state. Do not use kernel `!thread` in this user-mode session.

## Experiment 2: Polling Wait

**Source:** `polling_wait.cpp`

### Objective

Observe the CPU cost of repeatedly calling `WaitForSingleObject(event, 0)` while the event remains unsignaled.

### Execution

The program prints the PID, main TID, and worker TID. The worker is named `polling-event-check`; the coordinator is named `polling-coordinator`.

A preparation event confirms worker entry. The worker initially blocks on the same type of start gate used by the blocking example, allowing tools to be prepared without creating polling load.

Main samples the worker's CPU and cycle counters, then opens the gate. A second event acknowledges that the worker is approaching the polling routine.

`PollUntilSignaled` repeatedly performs a zero-timeout wait. When the result is `WAIT_TIMEOUT`, the worker immediately polls again. The loop uses a monotonic-clock safety deadline and does not print to the console on every iteration.

Main leaves the completion event unsignaled for the observation interval and waits on the worker handle. Early worker termination is a failure.

After the interval, main signals completion, joins the worker, verifies the exit code and event observation, checks that the poll count is nonzero, and collects the final counters.

The program reports:

- Worker user-mode CPU time.
- Worker kernel-mode CPU time.
- Total CPU-time delta.
- Thread cycle-count delta.
- Poll count.
- Measured gate-to-join wall-clock time.

The join ensures that the worker has terminated before main reads the final poll count. A positive count proves that the polling routine ran; it does not establish a required polling rate.

### Process Explorer

Start Process Explorer before launching the program. At the preparation prompt, locate the PID, open **Properties → Threads**, and identify the worker using its printed TID.

Display the available CPU, CPU Time, and Cycles Delta columns. Press Enter to begin the observation interval and inspect the worker's stack.

Look for `PollUntilSignaled` and `PollingEventWorker`. A sampled stack may instead show the zero-timeout wait API or the monotonic-clock check.

Compare the worker's CPU-time change with a separate blocking-wait run under similar conditions. Normally, the poller accumulates more worker CPU time because it repeatedly executes code while the event remains unsignaled.

A single worker can still represent a small percentage of total machine CPU usage, especially on a many-CPU system.

### WinDbg

Launch the program under x64 WinDbg or attach during the preparation checkpoint.

```text
.symfix
.sympath+ C:\path\to\this\lab
.reload /f
~
~~[HEX_TID]s
k
!runaway 7
x polling_wait!*PollUntilSignaled*
bu polling_wait!PollUntilSignaled
g
```

Replace `HEX_TID` with the printed hexadecimal worker TID, without the `0x` prefix.

After pressing Enter in the console, inspect the routine when the breakpoint is reached:

```text
k
dv /t /v
u polling_wait!PollUntilSignaled
bl
```

Clear only the lab breakpoint using its number from `bl`, or use `bc *` if you deliberately intend to remove all breakpoints in this dedicated lab session. Continue execution with `g`.

Do not leave breakpoints enabled on every poll. Repeatedly single-stepping the loop changes the behavior being measured. Debugger pauses can also exhaust the program's safety deadlines.

`k` and `!runaway` provide user-mode observations, not a complete scheduler-state timeline.

## Comparing the Two Experiments

Run both programs separately under similar system conditions.

```bat
blocking_wait.exe --auto
polling_wait.exe --auto
```

Use normal mode when more observation time is needed for GUI tools.

| Property | Blocking wait | Polling wait |
|---|---|---|
| Wait timeout | Finite, nonzero | Zero |
| Behavior while event is unsignaled | Blocks in the wait | Repeatedly checks the event |
| Repeated worker execution | Normally minimal during the wait | Continues polling while scheduled |
| Expected worker CPU consumption | Usually much lower | Usually higher |
| Completion mechanism | Completion event | Completion event |
| Event type | Unnamed, manual-reset | Unnamed, manual-reset |
| Worker CPU and cycle counters | Collected | Collected |

Compare the printed worker CPU-time and cycle-count deltas, not just elapsed time. Both programs include gate, handshake, scheduling, and shutdown overhead in their measured intervals.

There is no required CPU threshold, polling rate, or fixed performance ratio. Repeat the runs under similar load before drawing conclusions.

## WPR and WPA

For a more detailed scheduler analysis, use Windows Performance Recorder and Windows Performance Analyzer with the Windows Performance Toolkit installed.

Record each program in a separate trace:

```bat
wpr -start GeneralProfile -filemode
blocking_wait.exe
wpr -stop blocking-wait.etl
wpa blocking-wait.etl
```

Repeat for the polling program, using a different trace filename.

Do not stop or replace another analyst's active WPR recording. Verify that the selected recording profile includes the context-switch and ready-thread events needed for the analysis.

In WPA, use **CPU Usage (Precise)** and filter by the printed PID and worker TID. Select the actual observation interval rather than the preparation pause.

Inspect switch-out, ready, and switch-in timing. CPU Usage (Sampled) can help identify active code, but a blocked worker may generate very few samples.

Keep PDB files alongside their matching binaries for stack resolution.

Procmon can provide thread creation and exit events for lifetime correlation, but it does not provide a complete scheduler trace.

## Interpretation and Caveats

- **Blocking is not immediate execution after signaling.** Signaling the event makes the wait satisfiable. The worker may become Ready and still wait before it executes again.
- **Polling is not blocking.** `WAIT_TIMEOUT` with a zero timeout is an immediate test result. It does not mean the thread spent the observation interval blocked.
- **`Sleep(0)` is not an event wait.** It yields the caller's remaining time slice to an eligible thread, but does not make progress conditional on the event becoming signaled. The caller may resume immediately.
- **CPU time and elapsed time measure different things.** A worker can spend several seconds waiting while accumulating almost no CPU time.
- **Cycle counts are not energy measurements.** They depend on platform and power behavior and must not be converted to elapsed seconds using an assumed clock rate.
- **Counters are not a scheduler trace.** `GetThreadTimes` has accounting granularity, and separate CPU-time and cycle-count samples are not an atomic snapshot.
- **Tools affect the experiment.** Background load, virtualization, power management, tracing, and debugger activity can alter the results.
- **Timeouts are safety limits, not real-time guarantees.** Start gates are bounded at 40 seconds in normal mode or 6 seconds in automatic mode. Completion waits and joins are also bounded. Severe starvation, suspension, or prolonged debugger pauses can cause a checked failure.
- **Failures do not leave an intentionally detached worker.** The programs report fatal errors and exit with code `1`. They do not use `TerminateThread`. Windows reclaims process resources on termination.
- **Build success is not runtime validation.** Cross-compilation alone does not establish that the examples work correctly on native Windows.

## Check Yourself

1. Why can a blocking worker accumulate almost no CPU time while several seconds elapse?
2. What does the approach event establish, and what scheduler fact does it deliberately not establish?
3. Why can a stack contain a wait API even when the worker is actively polling?
4. Which trace interval distinguishes event satisfaction from the worker's next execution?
5. Why does adding `Sleep(0)` fail to express a dependency on an event?
6. Why is a CPU-time delta insufficient to reconstruct Ready, Running, and Waiting transitions?

## References

- [WaitForSingleObject](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject)
- [Using Event Objects](https://learn.microsoft.com/en-us/windows/win32/sync/using-event-objects)
- [GetThreadTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreadtimes)
- [QueryThreadCycleTime](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime)
- [Sleep](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleep)
- [GetTickCount64](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-gettickcount64)
- [Process Explorer](https://learn.microsoft.com/en-us/sysinternals/downloads/process-explorer)
- [CPU analysis in WPA](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/cpu-analysis)
- [WPR command-line options](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/wpr-command-line-options)
- [SetThreadDescription](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreaddescription)
- [_beginthreadex](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/beginthread-beginthreadex)

