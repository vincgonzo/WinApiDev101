# Thread Birth

This lab explores Windows thread creation using the Win32 `CreateThread` API. It compares a thread created in the normal state with one created using `CREATE_SUSPENDED`.

The objective is to distinguish thread creation, routine entry, and actual execution scheduling.

## Files

- `immediate_start.cpp` — Creates a worker without `CREATE_SUSPENDED`.
- `suspended_start.cpp` — Creates a worker with `CREATE_SUSPENDED` and resumes it explicitly.

## Requirements

- Windows 10 version 1607 or later / Windows 11
- x64 Visual Studio C++ build tools and Windows SDK
- Optional: WinDbg, Process Explorer, Process Monitor (Procmon)

Both examples use Win32 APIs in the worker thread.

## Build

Run the commands from an x64 Native Tools Command Prompt in this directory.

**Immediate start**

```bat
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc immediate_start.cpp /Fe:immediate_start.exe /link /DEBUG
immediate_start.exe
immediate_start.exe --auto
```

**Suspended start**

```bat
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc suspended_start.cpp /Fe:suspended_start.exe /link /DEBUG
suspended_start.exe
suspended_start.exe --auto
```

`/Od` disables optimization and `/Zi` generates debugging information for inspection.

## 1. Immediate Start

**Source:** `immediate_start.cpp`

### Learning question

Can the worker execute before the creator finishes the `CreateThread` call?

Yes. Without `CREATE_SUSPENDED`, the worker may enter its routine before `CreateThread` returns. It may also start later. The API does not guarantee a scheduling order.

### Observe

1. Run the program and record the PID, TID, and entry-flag snapshot.
2. Repeat the experiment. You do not need to observe both snapshot values.
3. Wait for the worker-generated entry event.
4. Open Process Explorer and locate the worker under **Properties → Threads**.
5. Inspect its stack.
6. Press Enter to signal the stop event and join the worker.

The entry snapshot may be `0` or `1`. A value of `1` does not prove that entry happened before `CreateThread` returned; entry could have occurred between the API return and the snapshot.

The worker-generated event establishes that entry has occurred when the main thread's wait completes.

The worker remains available for inspection for up to 120 seconds after entry. The expected exit code is `42`.

### WinDbg

```text
bu immediate_start!WorkerMain
g
```

The breakpoint may be reached before the creator prints its first post-creation message.

Useful commands:

- `~` — List debugger threads.
- `k` — Display the current thread's stack.
- `r` — Display registers.
- `!teb` — Inspect the current thread's TEB.
- `g` — Continue execution.

## 2. Suspended Start

**Source:** `suspended_start.cpp`

### Learning question

Does successful thread creation prove that the application's thread routine has executed?

No. With `CREATE_SUSPENDED`, `CreateThread` returns a valid handle and TID, but the worker cannot enter `WorkerMain` until its suspend count reaches zero.

### Observe

1. Run the program and leave the first checkpoint open.
2. Record the PID, TID, handle, and zero entry flag.
3. Locate the worker in Process Explorer under **Properties → Threads**.
4. Press Enter to resume the worker.
5. Wait for the entry event and inspect the worker's stack.
6. Press Enter to stop the worker and join it.

A listed thread proves existence, not execution of its application routine.

`ResumeThread` returns the previous suspend count. A return value of `1` means the call removed the last suspension. It does not prove that `WorkerMain` has already executed.

The worker normally returns `42`. If inspection exceeds the 120-second safety timeout after entry, exit code `124` is expected.

### WinDbg

At the initial debugger break:

```text
.symfix
.sympath+ C:\path\to\this\directory
.reload
bu suspended_start!WorkerMain
g
```

At the first console checkpoint, press Enter to resume the worker and trigger the breakpoint.

Use `~~[HEX_TID]s` to select the worker by its hexadecimal OS TID, without the `0x` prefix. Debugger thread indexes are not OS thread IDs.

The stack may contain Windows startup wrappers such as `BaseThreadInitThunk` and `RtlUserThreadStart`. Exact frames vary. Use `g` to continue execution before interacting with the console.

For direct `ETHREAD` or `KTHREAD` inspection, use a separate kernel-debugging VM and follow `TOOLS.md`. User-mode `!teb` does not display an `ETHREAD`.

## Procmon and Scheduling

Start Procmon capture before launching the program. Filter on the executable name and inspect `Thread Create` and `Thread Exit` operations.

Procmon provides thread-lifecycle telemetry, not a trace of the worker's first user-mode instruction or a complete scheduler history.

WPR/WPA can help correlate thread running intervals. Precise instruction-level ordering requires appropriately placed instrumentation or debugger evidence.

Debugger breaks and instrumentation can change timing. Internal thread-creation paths may also vary across Windows versions.

## Comparison

| Property | Immediate start | Suspended start |
|---|---|---|
| Creation flag | `0` | `CREATE_SUSPENDED` |
| Thread exists after successful creation | Yes | Yes |
| Worker may enter before `CreateThread` returns | Yes | No, while suspended |
| Entry flag at initial observation | `0` or `1` | `0` |
| Explicit `ResumeThread` required | No | Yes |
| Entry event used as evidence | Yes | Yes |
| Typical worker exit code | `42` | `42` |

## Analyst Takeaways

- **Creation is not execution.** A valid handle and TID do not prove routine entry.
- **Use synchronization instead of delays.** An event provides stronger evidence of progress than a short `Sleep`.
- **Initialize shared state first.** A runnable worker may access shared data before the creator executes its next statement.
- **Distinguish observation from inference.** A snapshot cannot establish an exact ordering of earlier events.
- **Account for observer effects.** Debugging can alter scheduling and timing.
- **Separate API guarantees from implementation details.** Internal Windows behavior can vary between versions.

## Check Yourself

1. Which observation proves thread existence?
2. Which observation proves entry into `WorkerMain`?
3. Why is an event stronger evidence than a short `Sleep`?
4. Can an entry snapshot of `1` prove that entry preceded the return of `CreateThread`?
5. What does a return value of `1` from `ResumeThread` establish?
6. What can Procmon prove about thread creation, and what remains unproven?

## References

- [CreateThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createthread)
- [ResumeThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-resumethread)
- [WaitForSingleObject](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject)
- [SetEvent](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setevent)
~
~
~
~
~
~
