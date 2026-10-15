# GUI Thread Responsiveness: Blocking Work vs. Worker Threads

**Companions:** `blocking_ui.cpp` and `worker_ui.cpp`
**Navigation:** Training overview · Tool setup

## Learning objectives

Understand how performing finite CPU work on a GUI thread affects message dispatch, responsiveness, and application shutdown.

This exercise compares two implementations of the same checksum computation:

- **Blocking UI thread:** Performs the computation directly inside the `WM_COMMAND` handler, preventing the UI thread from processing messages until the handler returns.
- **Worker thread:** Performs the computation on a dedicated worker while the UI thread continues processing messages, updating the heartbeat, painting the window, and handling cancellation requests.

Both programs use a native Win32 window with a Start button and fields for process/thread identity, status, elapsed time, heartbeat, and checksum results.

The key distinction is that **finite work can still block a GUI thread**. Responsiveness depends not only on how much work is performed, but also on which thread performs it.

## Repository structure

```text
12_GUI_Thread_Responsiveness/
├── README.md
├── blocking_ui.cpp
└── worker_ui.cpp
````

## Environment and build

Requirements: Windows 10 version 1607 or later, or Windows 11, with the x64 Windows SDK and C/C++ runtime.

Open an x64 Native Tools Command Prompt in the directory containing the source files.

### Build and run the blocking implementation

bat
```
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc blocking_ui.cpp /Fe:blocking_ui.exe /link /DEBUG user32.lib
blocking_ui.exe
blocking_ui.exe --auto
```

### Build and run the worker-thread implementation

bat
```
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc worker_ui.cpp /Fe:worker_ui.exe /link /DEBUG user32.lib
worker_ui.exe
worker_ui.exe --auto
```

### Visual Studio configuration

For either program:

1. Create an x64 Console App.
2. Disable precompiled headers.
3. Replace the generated source with the corresponding `.cpp` file.
4. Select C++17 and enable `/Od`, `/Zi`, and linker `/DEBUG`.
5. Add `user32.lib` under Linker \> Input \> Additional Dependencies.
6. Keep the Console subsystem and the ordinary `main` entry point.
7. Retain the matching PDB file for debugging.

Both programs deliberately use a console and a native Win32 window. A Windows-subsystem entry point such as `WinMain` is not required.

Neither program requires third-party libraries, external assets, or shared training headers. Both resolve `SetThreadDescription` dynamically from `KernelBase.dll` to support the stated Windows compatibility target.

### Window layout

Both applications start with a 720 × 420 logical-pixel window and a minimum size of 600 × 400. Controls use the standard Windows font and appearance. Separate rows and resize-adjusted label widths keep identity, status, timing, heartbeat, and results visible.

### Execution modes

| Mode | Behaviour |
| --- | --- |
| Normal | Provides an interactive Start button and allows inspection with Process Explorer, WPA, or WinDbg. |
| `--auto` | Starts the computation automatically, validates the expected evidence, and closes the window automatically. |

The computation is finite and normally completes within the exercise's 20-second budget. Automatic mode uses approximately one second of computation, while normal mode uses approximately four seconds.

These timings are not hard real-time guarantees. Debugger stops, system load, and scheduler delays can affect execution.

Both programs return process exit code `0` on success and `1` on API failures or violated invariants.

## Experiment 1 — Finite work can still block a GUI thread

Source: `blocking_ui.cpp`

### Purpose

Demonstrate that performing CPU work directly inside a GUI message handler prevents that thread from processing other window messages.

The application creates a native Win32 window and pumps messages on its main UI thread. A nominal 100 ms `WM_TIMER` advances the visible heartbeat.

When Start is clicked, the program performs approximately four seconds of finite checksum computation directly inside the `WM_COMMAND` handler.

During the computation, the handler does not pump messages. Consequently, timer processing, normal painting, input handling, and close handling cannot proceed until the handler returns.

The computation uses one logical CPU during its bounded execution. There is no affinity change, priority adjustment, remote thread, or forced termination.

### Controlled sequence

1. Main names the UI thread `blocking-ui`.
2. The program prints the PID and UI TID in decimal and hexadecimal.
3. The application creates the window and starts its message loop.
4. The heartbeat advances while the UI thread processes timer messages.
5. Click Start.
6. The application displays its initial computing status and calls `ComputeFor` directly inside `WM_COMMAND`.
7. The checksum computation runs for approximately four seconds, checking elapsed time between fixed-size batches.
8. While the handler executes, the heartbeat and elapsed-time display stop updating.
9. The computation finishes, and its result is displayed and printed.
10. Message dispatch resumes, allowing pending timer, paint, input, and close messages to be processed.

The recorded heartbeat delta during the blocking handler is zero. The next timer update can reveal a larger gap between visible heartbeats.

Normal mode leaves the completed result visible and permits another run.

### Expected evidence

| Observation | Expected result |
| --- | --- |
| UI thread | `blocking-ui` |
| Computation location | UI thread |
| Worker thread created for computation | No |
| Heartbeat during computation | Stops advancing |
| Timer dispatch during computation | Blocked |
| Normal painting and close handling | Delayed |
| Checksum result | Nonempty, valid result |
| Process exit code | `0` on success |

### Observe the blocked window

Try the following while the computation is running:

1. Click Start.
2. Observe the heartbeat and elapsed-time fields.
3. Attempt to resize or move the window.
4. Try interacting with the window or clicking its close button.
5. Wait for the finite computation to finish.

The application's normal message handling is delayed until the computation returns.

The desktop compositor may still move a cached window surface or display other operating-system-managed visuals. Such movement does not prove that the application is dispatching messages.

Do not use an operating-system End task action for this exercise. The computation finishes on its own.

### Understand the heartbeat

The heartbeat advances through `WM_TIMER` processing. It is an observation of message dispatch, not a direct measurement of CPU scheduling.

A frozen heartbeat does not mean that the UI thread is waiting. In this example, the thread is actively executing CPU work inside the command handler.

Timer messages can coalesce, so the number of timer callbacks is not guaranteed to match an exact 100 ms interval.

### Inspect with Process Explorer

While the computation is running:

1. Open the process properties for the printed PID.
2. Select Threads.
3. Find the UI thread using the printed TID.
4. Inspect its stack.

Look for `ComputeFor`, `StartWork`, and `WindowProc` beneath Windows message-dispatch frames.

A stack captured after completion will more likely show an idle message wait. The thread's start address identifies its origin, not the computation currently executing on it.

### Inspect with WPR/WPA

An optional Windows Performance Recorder trace with CPU Usage enabled can help identify where the computation executes.

In Windows Performance Analyzer, examine CPU activity and associate the checksum work with the UI TID.

One busy logical processor may represent only a small percentage of total CPU usage on a many-core system. The window can still be unresponsive because its message-processing thread is occupied.

Procmon can show process and thread events, but it does not identify which `WM_COMMAND` handler is preventing message dispatch. This program does not create a separate worker thread for the computation.

### Inspect with WinDbg

Launch the normal executable under x64 WinDbg with its matching PDB.

text
```
.symfix
.sympath+ C:\training\12_GUI_Thread_Responsiveness
.reload
x blocking_ui!*ComputeFor*
bu blocking_ui!ComputeFor
g
```

Click Start. At the `ComputeFor` breakpoint, inspect the current thread and stack.

text
```
~
~.
k
r rip
ln @rip
dv /t
g
```

The current OS TID should match the UI TID. The stack should show the computation beneath the command handler rather than beneath a separate worker entry point.

To select the UI thread explicitly at a later break:

text
```
~~[HEX_TID]s
k
lm
!address @rip
```

Replace `HEX_TID` with the printed hexadecimal UI TID without the `0x` prefix.

The `~` command displays debugger thread indexes, not OS TIDs. Use `~~[HEX_TID]s` to select a thread by its OS TID.

A debugger breakpoint freezes the debugged process. Assess responsiveness with the debugger running, not while the process is stopped.

Long debugger stops can consume the elapsed-time budget and trigger the automatic watchdog. Prefer normal mode for interactive debugging.

### Automatic mode

Run:

bat
```
blocking_ui.exe --auto
```

The program posts the Start command after window creation, uses approximately one second of computation, continues pumping messages, observes a timer after completion, and posts Close automatically.

It validates a nonempty result, zero heartbeat advancement inside the handler, and an observed heartbeat before shutdown.

The 12-second automatic watchdog is checked by the timer after message dispatch resumes. It cannot interrupt a blocked handler, so the finite computation itself is the primary execution bound.

## Experiment 2 — Keep the window responsive while a worker computes

Source: `worker_ui.cpp`

### Purpose

Run the same finite checksum computation while allowing the UI thread to continue dispatching messages.

The application creates one owned worker thread to perform the computation. While the worker runs, the UI can update its heartbeat, process input, repaint controls, and display elapsed time.

Close requests cooperative cancellation. The application keeps the window and worker context alive until the worker has stopped accessing them and its resources have been cleaned up.

This is an SDK/CRT-only teaching program, not a framework application. It uses `_beginthreadex` to create a CRT-compatible worker.

All window and control mutations remain on the UI thread. The worker's only window-directed operation is an asynchronous `PostMessageW(WM_APP + 1, runNumber, 0)` completion notification.

The worker does not use synchronous cross-thread `SendMessage` or wait for UI work.

### Controlled sequence

1. Main names the UI thread `worker-ui`.
2. The program prints the PID and UI TID in decimal and hexadecimal.
3. The application creates the window and starts its message loop.
4. Click Start.
5. The application creates a worker named `responsive-worker` and prints its TID.
6. The Start button remains disabled while the computation is active.
7. The worker performs fixed-size checksum batches for approximately four seconds, checking elapsed time and Interlocked cancellation between batches.
8. The UI continues processing timer, input, and paint messages.
9. The worker stores its result, posts a completion message, and returns.
10. The UI waits until the worker thread handle is signaled before reading the final result fields, validating the TID and exit code, and closing the handle.
11. Normal mode retains the completed results and allows another run.

The completion message alone does not prove that the worker has finished returning through the CRT. The thread handle becoming signaled provides the synchronization point used before the UI reads the worker's final data.

### Expected evidence

| Observation | Expected result |
| --- | --- |
| UI thread | `worker-ui` |
| Worker thread | `responsive-worker` |
| UI and worker TIDs | Different |
| Computation location | Worker thread |
| UI message dispatch during computation | Continues |
| Heartbeat during computation | Advances |
| Painting and input handling | Remain responsive |
| Cancellation | Cooperative |
| Process exit code | `0` on success |

The worker version ordinarily records multiple heartbeat updates while work is active. No exact heartbeat count or elapsed duration is asserted.

Because the workload is elapsed-time-based, checksum values and batch counts need not match between runs or between the two programs. The results demonstrate finite work, not benchmark performance.

### Thread ownership and completion

The worker uses a `WorkContext` that remains alive for its entire lifetime.

The cancellation field uses Interlocked operations. Other worker-written result fields are read only after the thread handle is signaled, not merely because a completion message has arrived.

The displayed result is a separate UI-owned copy. Run numbers identify completion messages.

The message loop uses `MsgWaitForMultipleObjectsEx` and `PeekMessageW` so that both queued messages and worker termination can wake the UI thread.

The program checks the `WAIT_FAILED` case and does not rely on a `while (GetMessage(...))` loop that could accidentally treat `-1` as a valid message.

The UI's worker-join check uses a zero timeout, allowing message pumping to continue while the worker finishes returning.

### Cooperative shutdown

When Close is requested during active work:

1. The UI requests cancellation through the Interlocked cancellation field.
2. The window and worker context remain alive.
3. The UI continues pumping messages while the worker responds to cancellation.
4. The worker finishes and signals thread termination.
5. The UI reads the final result and exit status, cleans up the worker handle, and destroys the window.

No thread is forcibly terminated, and no worker-owned UI call can deadlock the join path.

The window is not destroyed while the worker may still access its context or post completion notifications.

### Error handling

The worker's completion notification is asynchronous. If `PostMessageW` fails, the signaled worker handle can still wake the UI.

The UI then reads the recorded error after worker termination, performs cleanup safely, and returns failure instead of waiting indefinitely for a message that will never arrive.

If `_beginthreadex` fails, no worker exists to join. The program reports the error, closes the window, and returns failure.

The initial automatic Start post is checked as well.

Normal-mode Close during a run is a successful cooperative cancellation, not a forced-thread shutdown. Unexpected infrastructure failures are reported as failures.

### Inspect with Process Explorer

While the computation is running:

1. Open the process properties for the printed PID.
2. Select Threads.
3. Locate `worker-ui` and `responsive-worker` using their printed TIDs.
4. Inspect both stacks.

The worker should be in or near `ComputeFor` or `ComputeWorker`. The UI thread should be dispatching messages or waiting for messages and handles.

A UI stack does not have to contain a timer callback at the instant of inspection. Its current frames depend on where the snapshot lands.

The worker's Start Address may point to a CRT startup wrapper rather than `ComputeWorker`. Thread creation provenance is not the same as current execution behaviour.

### Inspect with WPR/WPA

An optional WPR CPU Usage trace should attribute the checksum work to the worker TID, with smaller UI activity visible separately.

Procmon can show worker creation and exit but does not reconstruct every callback or message.

Try the following:

1. Click Start.
2. Resize the window while the heartbeat advances.
3. Observe the elapsed-time display.
4. Click Close during active work.
5. Repeat the same experiment with `blocking_ui.exe`.

The compositor can move cached window surfaces independently of application message dispatch. The heartbeat and actual thread stacks provide stronger evidence of responsiveness than window movement alone.

### Inspect with WinDbg: computation and completion

Launch the normal executable under x64 WinDbg.

text
```
.symfix
.sympath+ C:\training\12_GUI_Thread_Responsiveness
.reload
x worker_ui!*ComputeWorker*
x worker_ui!*ComputeFor*
bu worker_ui!ComputeFor
bu worker_ui!TryFinishWorker
g
```

Click Start. At the `ComputeFor` breakpoint, inspect the worker's execution context.

text
```
~
~.
k
r rip
ln @rip
dv /t
g
```

`TryFinishWorker` may break while the worker is still active because it also performs a nonblocking completion check.

Use the following commands to inspect and remove breakpoints after collecting evidence:

text
```
bl
bc *
g
```

At a breakpoint, compare the UI and worker OS TIDs explicitly.

text
```
~~[HEX_WORKER_TID]s
k
~~[HEX_UI_TID]s
k
g
```

Replace the placeholders with the printed hexadecimal TIDs without the `0x` prefix.

Debugger indexes used by `~Ns` are not OS TIDs. Select the worker before it exits, because an exited worker may no longer be selectable.

User-mode thread and stack inspection does not directly expose kernel `ETHREAD` or `KTHREAD` objects.

A debugger stop freezes UI progress too. Test responsiveness only while execution is continued.

### Automatic mode

Run:

bat
```
worker_ui.exe --auto
```

The program posts Start automatically, performs approximately one second of computation, verifies a completed nonempty result and at least one heartbeat while work was active, then closes automatically after joining the worker.

No exact heartbeat count or duration is asserted.

A 12-second automatic watchdog requests cancellation while the UI continues pumping messages. Ordinary automatic execution is comfortably below 20 seconds, but this is not a hard scheduling guarantee.

Debugger stops or severe contention can invalidate automatic timing checks. Use normal mode for debugging.

## Direct comparison

| Property | Blocking UI thread | Worker thread |
| --- | --- | --- |
| Source file | `blocking_ui.cpp` | `worker_ui.cpp` |
| Computation function | `ComputeFor` | `ComputeFor` / `ComputeWorker` |
| Computation thread | UI thread | Dedicated worker |
| UI heartbeat during computation | Stops advancing | Continues advancing |
| Timer message dispatch | Blocked during computation | Continues |
| Painting and input | Delayed | Remain responsive |
| Close during work | Delayed until handler returns | Requests cooperative cancellation |
| Worker creation | None for the computation | `_beginthreadex` |
| Completion handling | Handler returns to message loop | Asynchronous completion notification plus thread-handle signaling |
| Result synchronization | Computation completes in the UI handler | UI reads worker results after thread termination |
| Thread cleanup | No computation worker to join | Worker context and handle cleaned up after termination |
| Forced termination | Not used | Not used |
| Main lesson | Finite work can block a GUI thread | Moving work off the UI thread preserves message dispatch |

The two programs intentionally use the same checksum algorithm and broadly similar workload duration. The primary difference is ownership of the computation, not the algorithm.

## Important implementation details

### Message dispatch and responsiveness

The blocking implementation executes CPU work directly in a window-message handler. While that handler is running, the UI thread cannot dispatch other queued messages.

The worker implementation lets the UI thread return to its message loop while the worker computes independently.

### Cancellation and lifetime

Cooperative cancellation requires the worker to check a shared cancellation request and finish safely. It does not forcibly interrupt arbitrary instructions.

The worker's context, window, and associated resources must remain valid until the worker has stopped accessing them.

### Synchronization and result access

A posted completion message is a notification, not proof that the worker has fully terminated.

The worker implementation waits for the thread handle to become signaled before reading its final result fields and cleaning up the handle. Interlocked operations protect the shared cancellation state.

### Timing limitations

- Timer callbacks are message-dispatch observations, not a complete scheduler trace.
- Timer messages can coalesce.
- A busy logical CPU may represent a small percentage of total CPU usage on a many-core system.
- Debugger stops can freeze the entire process, including the UI.
- Scheduler delays can affect elapsed-time measurements and automatic watchdog checks.
- User-mode stacks and thread lists do not provide direct access to kernel `ETHREAD` or `KTHREAD` objects.

Finite work can still be unacceptable on an event-loop thread. Pumping arbitrary messages inside the computation is not the solution demonstrated here because it introduces reentrancy. The worker implementation moves computation to another thread while retaining UI ownership of controls, result presentation, and shutdown.

## Learner questions

1. Why is zero heartbeat advancement compatible with high CPU usage?
2. Why can a short command handler make the entire client area unresponsive?
3. Which stack frames distinguish the blocking implementation from the worker implementation?
4. Why is an exact number of 100 ms timer callbacks an invalid expectation?
5. Why does a completion message not prove that the worker has fully terminated?
6. Which synchronization event makes it safe to read the worker's result fields?
7. Why must Close keep the window and worker context alive until the worker has stopped?
8. How can the UI recover when the worker's completion post fails?
9. Why is a zero-time worker-completion check preferable to blocking the UI for the entire job?
10. Why is moving computation to a worker different from pumping arbitrary messages inside the computation?
11. Why can window movement alone not prove that the UI thread is responsive?
12. What evidence from Process Explorer or WPA establishes which thread performed the checksum work?

## Official references

- [Using Messages and Message Queues](<https://learn.microsoft.com/en-us/windows/win32/winmsg/about-messages-and-message-queues>)
- [GetMessageW](<https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getmessagew>)
- [WM\_COMMAND](<https://learn.microsoft.com/en-us/windows/win32/menurc/wm-command>)
- [WM\_TIMER](<https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-timer>)
- [Preventing Hangs in Windows Applications](<https://learn.microsoft.com/en-us/windows/win32/win7appqual/preventing-hangs-in-windows-applications>)
- [SetThreadDescription](<https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreaddescription>)
- [\_beginthread and \_beginthreadex](<https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/beginthread-beginthreadex>)
- [PostMessageW](<https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-postmessagew>)
- [MsgWaitForMultipleObjectsEx](<https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-msgwaitformultipleobjectsex>)
- [PeekMessageW](<https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-peekmessagew>)
- [WaitForSingleObject](<https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject>)
- [DestroyWindow](<https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-destroywindow>)
- [InterlockedExchange](<https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-interlockedexchange>)
- [x64 Calling Convention](<https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention>)

## Key takeaway

A GUI thread must remain available to dispatch messages if the application is to remain responsive. The blocking implementation performs finite work on the UI thread and delays timer, paint, input, and close handling. The worker implementation moves the computation to an owned worker while keeping UI updates and shutdown on the UI thread.

Compare the heartbeat, thread IDs, stack traces, completion synchronization, and cancellation behaviour to understand why moving CPU work off the UI thread improves responsiveness without sacrificing safe resource management.

