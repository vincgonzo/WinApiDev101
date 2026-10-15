# Blocking On An Event

## Objective

Observe a thread that asks Windows to wait for an unsignaled event instead
of repeatedly asking whether it is signaled. Compare its worker CPU-time
and cycle deltas with [polling_wait](polling_wait.README.md). Both programs
create the **same kind** of object: an initially unsignaled, unnamed,
manual-reset event. They do not share an event across processes.

See [the bundle guide](../README.md) and [tool setup](../TOOLS.md). This
source is a standalone console program using only the Windows SDK and CRT.

## Build And Run

In an **x64 Native Tools Command Prompt for Visual Studio**:

```bat
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc blocking_wait.cpp /Fe:blocking_wait.exe /link /DEBUG
blocking_wait.exe
blocking_wait.exe --auto
```

The native target is Windows 10 1607+ or Windows 11 x64. No third-party
library or elevation is needed for the executable. Names use
`SetThreadDescription`, resolved from KernelBase at runtime for Windows 10
1607 compatibility. Naming/API failures are checked. CRT-using worker
code starts with `_beginthreadex`.

Normal mode prints a **pre-run** tool-preparation prompt, with a 30-second
limit and Enter to continue, then observes for approximately five seconds.
`--auto` skips the prompt and observes for approximately one second; it is
intended to complete in under 20 seconds. Non-console input skips the
prompt too. Extra or unknown arguments fail with a usage message. Exit
codes are 0 for success and 1 for an error or failed check; no unsupported
hardware case uses 77 in this demo.

## Run Phases And Checks

1. Main prints its PID/TID and the worker TID in decimal and hex. The
   worker is named `blocking-event-wait`; the coordinator is
   `blocking-coordinator`. A prepared event acknowledges worker entry. The
   worker then waits at a separate common-to-the-run start gate.
2. Prepare tools at the prompt. Main samples `GetThreadTimes` and
   `QueryThreadCycleTime` through the retained worker handle, starts the
   wall-clock interval, and opens the start gate.
3. `BlockingEventWorker` signals an approach event immediately before
   calling `WaitForSingleObject(completion, finite_timeout)`. This is an
   explicit handshake, **not proof** that Windows has already placed the
   thread in the Waiting state. No `Sleep` is used to guess its state.
4. Main keeps the completion event unsignaled while it waits five seconds
   (one in auto mode) on the worker handle. That wait must time out: early
   worker termination is a failure. Ordinarily the worker spends nearly
   all this interval blocked in the event wait, with little charged CPU.
5. Main signals the event, joins the worker, checks its recorded
   `WAIT_OBJECT_0` and zero exit code, then collects final counters before
   closing the worker handle. All other event handles are closed as well.

The printed CPU delta separates user and kernel milliseconds, then sums
them. The cycle delta and measured gate-to-join wall time are also shown.
The interval includes gate release, acknowledgement, scheduling, and
shutdown overhead; it is not an instrumented duration of only the kernel
wait. Counter monotonicity and successful completion are checked. **No
fixed CPU or cycle threshold is a pass/fail condition.**

## Process Explorer

1. Start Process Explorer before running the demo. At the preparation
   prompt, find the printed decimal PID and open **Properties > Threads**.
   Identify the printed worker TID, not the main thread or an assumed row.
2. Where your Process Explorer version supports them, display **CPU**,
   **CPU Time**, and **Cycles Delta** in the Threads view. The latter is a
   per-refresh cycle delta, not the program's whole-interval measurement.
   Record the worker's CPU-time change over the run; use the program's
   counters when a column is unavailable. Process-wide CPU is a different
   denominator and includes the coordinator.
3. Return to the console and press Enter. During the five-second interval,
   select the worker and open **Stack**. With the EXE's PDB and Microsoft
   symbols configured, expect `BlockingEventWorker` with an event-wait
   path such as `WaitForSingleObjectEx`/`NtWaitForSingleObject`. Exact
   system frames vary by OS and tool version.
4. Run the polling companion separately under similar conditions. Compare
   its worker CPU-time delta and refresh-cycle activity to this run. The
   ordinary pattern is much less worker CPU here, not a promised ratio.

A stack captured at the initial prompt shows a start-gate wait. That is
not yet the measured event wait. Attaching tools has overhead, and a
snapshot is not a complete scheduling history.

## WinDbg (User Mode, x64)

Use the normal five-second mode for observation. Launch under WinDbg or
attach at the preparation prompt, configure symbols, and locate the
worker:

```text
.symfix
.sympath+ C:\path\to\this\lab
.reload /f
~
~~[1a2c]s
k
!teb
!runaway 7
x blocking_wait!*BlockingEventWorker*
g
```

Replace `1a2c` with the printed hexadecimal worker TID, without `0x`.
`~~[hex]s` selects an OS TID; `~1s` instead selects debugger index 1. After
pressing Enter in the console and allowing the run to begin, break in
briefly, select that TID again, and use `k` and `!runaway 7`. The latter
shows accumulated thread user/kernel/elapsed information; it does not
identify every Ready interval. Resume promptly with `g`.

There is no expectation that `!teb` proves a kernel scheduler state. These
are user-mode commands. Do not issue kernel `!thread` here; an optional
kernel debugger is a separate environment. Long debugger stops can exhaust
the demo's safety deadlines.

## WPA Or Command-Line Alternative

Capture both executables in **separate** comparable traces. From a terminal
authorized to record ETW, with Windows Performance Toolkit installed:

```bat
wpr -start GeneralProfile -filemode
blocking_wait.exe
wpr -stop blocking-wait.etl
wpa blocking-wait.etl
```

Do not stop or replace another analyst's active WPR recording. Verify the
chosen WPR profile includes context-switch and ready-thread events. In
WPA, filter **CPU Usage (Precise)** by the printed PID and worker TID, and
select the actual observation interval rather than the preparation pause.
Inspect switch-out, ready, and switch-in timing. **CPU Usage (Sampled)**
can show active code, but a blocked worker may have almost no samples.
Keep PDBs with their matching binaries for stack resolution.

For a simple CLI comparison, build the companion and run:

```bat
blocking_wait.exe --auto
polling_wait.exe --auto
```

Compare the printed **worker** user+kernel milliseconds and cycle deltas,
not just elapsed time. The normal five-second mode gives more samples for
GUI tools. Process Monitor thread creation/exit events establish lifetime
boundaries; they are not a full scheduler trace.

## Analyst Interpretation And Caveats

- An unsatisfied blocking wait allows Windows to run another eligible
  thread. Signaling its event makes the wait satisfiable; that does **not**
  prove the waiter executes instantly. It can become Ready and wait to be
  scheduled before it returns from the API. The join establishes eventual
  completion, not zero wakeup latency.
- This is a manual-reset event, so a successful wait does not reset it.
  The same event behavior is used by the polling companion.
- `Sleep(0)` yields the caller's remaining time slice to an eligible
  thread; it neither waits for this event nor puts the caller into an
  event-dependent wait. With no useful competitor the loop can resume
  immediately. Adding it to a poll loop is not equivalent to this program.
- `GetThreadTimes` reports CPU accounting, with platform-dependent
  granularity. Zero or small deltas are possible here. Cycle counts are
  hardware-dependent; do not convert them to elapsed seconds with one
  assumed clock rate or treat them as an energy measurement.
- Background load, power management, a VM, tracing, and debugger activity
  affect results. The pre-run prompt itself consumes no worker hot-loop
  CPU. A satisfied wait and sampled counters alone cannot reconstruct
  Ready/Running/Waiting transitions; use the precise scheduler trace.
- Start gates are bounded at 40 seconds normally or 6 seconds in auto
  mode. Completion waits are bounded at 12/6 seconds; joins at 3 seconds.
  Main requests only 5/1 seconds of observation. Severe starvation,
  suspend/resume, or long debugger pauses can cause a reported timeout;
  these are not real-time guarantees.
- Fatal errors print a diagnostic and exit the process with 1. Windows
  reclaims process resources instead of leaving a worker alive against
  destroyed storage. The demo never uses `TerminateThread`. Cross-build
  success does not substitute for native Windows runtime verification.

## Learner Questions

1. Why is almost unchanged worker CPU time compatible with several seconds
   of elapsed time?
2. What does the approach event prove, and which scheduler fact does it
   deliberately not claim to prove?
3. Which trace interval would separate event satisfaction from the
   worker's next actual execution?

## Microsoft References

- [WaitForSingleObject](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject)
- [Using Event Objects](https://learn.microsoft.com/en-us/windows/win32/sync/using-event-objects)
- [GetThreadTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreadtimes)
- [QueryThreadCycleTime](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime)
- [Sleep, including zero-duration behavior](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleep)
- [Process Explorer](https://learn.microsoft.com/en-us/sysinternals/downloads/process-explorer)
- [CPU analysis in WPA](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/cpu-analysis)
- [WPR command-line options](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/wpr-command-line-options)
- [SetThreadDescription](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreaddescription)
- [_beginthreadex](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/beginthread-beginthreadex)