# Windows Thread Lifetime and Handle Management

This lab explores the relationship between Windows thread execution, thread handles, and kernel-object lifetime through two experiments:

- **Closing a handle while a thread is running:** demonstrate that closing one thread handle does not stop the executing thread, while a duplicated handle remains available for reliable joining and cleanup.
- **Retaining a handle after thread exit:** demonstrate that a thread can finish executing while a valid handle to its thread object remains open for exit-code queries and further inspection.

The objective is to distinguish a thread's execution lifetime from the lifetime of references to its thread object. Neither experiment proves when the final kernel-object reference disappears or when the object is ultimately deleted.

See [`TOOLS.md`](TOOLS.md) and the training overview for shared setup instructions.

## Requirements

- Windows 10 version 1607 or later, or Windows 11, x64.
- An x64 Native Tools Command Prompt with the Windows SDK.
- Process Explorer and, optionally, x64 WinDbg.
- Windows Performance Toolkit for WPR/WPA tracing.

Both examples use Win32 APIs and the Windows SDK/C runtime. Compile each source file separately. Keep the executable, source, and matching PDB together for debugging. Cross-compilation validates neither Windows runtime behavior nor object-lifetime observations.

## 1. Closing a handle while the worker continues

**Source:** `close_handle_while_running.cpp`

### Objective

Show that closing a thread handle is not a command to stop the executing thread.

The program duplicates the original handle, closes the original, and uses a causal event handshake to prove that the worker continues making progress afterward. The duplicate remains open for joining and lifetime management, so this is explicitly **not** a demonstration of removing the last thread handle.

### Build and run

```bat
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc close_handle_while_running.cpp /Fe:close_handle_while_running.exe /link /DEBUG
close_handle_while_running.exe
close_handle_while_running.exe --auto
```

Keep the matching PDB for source and stack inspection. The worker uses `_beginthreadex` because it accesses C++ atomic counters and result fields.

### Execution and expected evidence

1. Main prints the PID and main/worker TIDs in decimal and hexadecimal.
2. The worker, named `close-handle-worker`, increments an atomic progress counter, signals entry, and continues through bounded event waits with occasional additional increments. Main is named `close-handle-main`.
3. `DuplicateHandle` creates a second, non-inheritable handle to the same thread object in the same process, with equivalent access rights.
4. During phase 1, the program prints the original and duplicate handle values. The inspection checkpoint advances on Enter or after three seconds; automatic mode skips console input.
5. Main snapshots the progress counter and successfully closes the original handle. It clears its local handle variable and never uses the closed value again.
6. Only after the successful close does main signal the post-close request event. The worker receives the request, increments progress, records the acknowledged value atomically, and signals a separate acknowledgment event.
7. Main waits for the acknowledgment and verifies that the acknowledged value exceeds the pre-close snapshot. This establishes a causal sequence: successful close, request, worker progress, acknowledgment.
8. Phase 3 provides another inspection checkpoint while the worker remains active. Main then signals cooperative stop, joins through the duplicate handle, queries the worker's return value of `42`, and closes the duplicate and event handles.

The acknowledgment is stronger evidence than sleeping and assuming a counter should have changed. The worker cannot acknowledge the request before main signals it, and main does not signal the request until the original handle has been successfully closed.

The worker's return value of `42` is distinct from the process's exit code, which is `0` on success.

**Expected duration:** interactive runs normally finish within six seconds; automatic mode finishes promptly. Redirected or non-console input advances immediately. An 18-second shared deadline bounds waits, and the worker checks it during finite wait intervals.

All worker-accessed state has process lifetime. Progress and results are atomic, and event handles are not closed underneath an unjoined worker. If duplication fails, the original handle remains available for cooperative cleanup and joining. No `TerminateThread` or external-process action is used.

### What this experiment proves

- Closing the original handle does not stop the worker.
- The worker makes additional progress after that close.
- The duplicate remains available for joining and exit-code inspection.
- The experiment does not remove the final handle and does not prove final kernel-object deletion.

## 2. Execution ends before the retained thread handle closes

**Source:** `retained_thread_handle.cpp`

### Objective

Separate a thread's execution lifetime from the lifetime of references to its thread object.

The worker returns `42` cooperatively. Main waits for execution to finish, then retains a valid thread handle for further exit-code queries and object inspection. Closing that handle releases this program's reference; it does not prove that no other references exist or that final object deletion occurs at that instant.

### Build and run

```bat
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc retained_thread_handle.cpp /Fe:retained_thread_handle.exe /link /DEBUG
retained_thread_handle.exe
retained_thread_handle.exe --auto
```

Keep the matching PDB. This worker uses Win32 APIs and Interlocked operations, so `CreateThread` is appropriate. CRT console output is performed only by main.

### Execution and expected evidence

1. Main prints the PID, main and worker TIDs in decimal and hexadecimal, the worker handle, and the stop-event handle.
2. The worker, named `retained-handle-worker`, signals entry and waits for the cooperative stop event. Main is named `retained-handle-main`.
3. Main checks `STILL_ACTIVE` during the live phase. The first inspection checkpoint advances on Enter or after three seconds; automatic mode skips input.
4. Main signals the stop event. The worker returns normally with exit code `42`; it is not forcibly terminated.
5. Main successfully waits on the thread handle. The signaled wait establishes that execution has ended.
6. Main calls `GetExitCodeThread` and verifies the result. During phase 3, it retains the same handle for an additional checkpoint of up to six seconds and queries the exit code again.
7. Main confirms that the thread handle remains signaled, closes it once, clears the local handle variable, and closes the event handles after the worker has joined.

The process exits with code `0` on success, not `42`. The latter is the worker's return value.

`STILL_ACTIVE` alone is not a universally reliable completion test because a thread could itself return `259`. In this experiment, the worker deliberately returns `42`, and the successful wait on the thread handle is the authoritative completion check.

**Expected duration:** interactive runs take up to approximately nine seconds; automatic mode completes promptly. Redirected or non-console input advances immediately. An 18-second shared deadline bounds operational waits. Debugger pauses, VM pauses, and scheduler starvation can exceed the budget and cause a reported failure.

Global Win32 state and Interlocked error reporting avoid dangling worker arguments and data races on error paths. If joining fails, synchronization handles are not closed underneath a potentially running worker.

### What this experiment proves

- The worker's execution has ended, as established by a successful wait on its thread handle.
- The exit code can still be queried while the handle remains valid.
- A finished worker can disappear from a live-thread list while a handle to its thread object remains open.
- The experiment does not establish the final reference count or the instant of final object deletion.

## Shared analysis tools

Prepare symbols before launching either program, as described in `TOOLS.md`. Both programs print process/thread identities and handle values to support correlation between program output, debugger inspection, and traces.

### Process Explorer

Open the printed PID in Process Explorer and select **Properties → Threads**.

For `close_handle_while_running.exe`, identify `close-handle-worker` using its printed TID. Inspect the stack and activity before and after the original handle closes. The same worker should continue through its event-wait/progress loop.

For `retained_thread_handle.exe`, identify `retained-handle-worker` during phase 1. Inspect its stack for the cooperative event wait. After the worker exits, it may disappear from the live Threads tab; this does not imply that its thread object has no remaining references.

To inspect handles, select **View → Show Lower Pane**, then **View → Lower Pane View → Handles**. Locate the printed Thread handle values where exposed by the tool version and permissions.

- In the first experiment, both handles initially refer to the same thread object. After the original handle closes, the duplicate remains.
- In the second experiment, the retained handle remains open after execution ends.

Do not close handles through Process Explorer: main owns their cleanup. Debuggers and inspection tools may themselves acquire additional references, and handle visibility can vary with refresh timing and permissions.

### WinDbg

Launch each executable in x64 WinDbg with its matching PDB and configure symbols.

For `close_handle_while_running.exe`:

```text
.symfix C:\Symbols
.sympath+ C:\labs\08_Termination_And_Object_Lifetime
.reload /f
bu close_handle_while_running!ProgressWorker
g
```

For `retained_thread_handle.exe`:

```text
.symfix C:\Symbols
.sympath+ C:\labs\08_Termination_And_Object_Lifetime
.reload /f
bu retained_thread_handle!CooperativeWorker
g
```

These entry breakpoints help explain the source path. After the program prints its identities and reaches the relevant phase, break briefly and inspect the actual worker and handles.

For the first experiment:

```text
~
~~[HEX_TID]s
kn
!teb
!handle ORIGINAL_HANDLE f
!handle DUPLICATE_HANDLE f
bc *
g
```

During phase 3, inspect the worker and duplicate handle again:

```text
~~[HEX_TID]s
kn
!handle DUPLICATE_HANDLE f
g
```

Do not query the closed original handle to infer object lifetime: its numeric value may have been reused for an unrelated object.

For the second experiment, inspect the live worker during phase 1:

```text
~
~~[HEX_TID]s
kn
!teb
!handle PRINTED_THREAD_HANDLE f
bc *
g
```

After the worker exits, switching to its TID may fail because no live thread context remains. During phase 3, inspect the retained handle instead:

```text
!handle PRINTED_THREAD_HANDLE f
~
g
```

Replace placeholders with actual printed values. `~~[HEX_TID]s` uses hexadecimal OS TID digits without the `0x` prefix; `~2s` selects debugger thread index 2 instead. Handle values are process-local and are neither TIDs nor debugger indexes.

`!handle actualhandle f` shows the object information available for the current debugger and OS version. Do not query a handle after main closes it, because the value may be reused.

For optional kernel-level inspection in a dedicated debugging VM, use the shared tools guide. Kernel commands such as `!thread` and `!object` require actual kernel addresses; a numeric TID and the user-mode `!teb` output are not substitutes for an ETHREAD or object address. Neither a displayed handle count nor the closing of one handle proves final object deletion.

Debugger stops perturb timing and consume the deadline budget. Rerun for uninterrupted behavior and tracing.

### WPR/WPA scheduler tracing

Install Windows Performance Toolkit. Use an elevated recording console, check the status, and avoid interfering with another WPR session.

For `close_handle_while_running.exe`:

```bat
wpr -status
wpr -start GeneralProfile -filemode
close_handle_while_running.exe
wpr -stop "%TEMP%\close-handle-while-running.etl"
```

For `retained_thread_handle.exe`:

```bat
wpr -status
wpr -start GeneralProfile -filemode
retained_thread_handle.exe
wpr -stop "%TEMP%\retained-thread-handle.etl"
```

In WPA, filter thread activity by the printed PID and TID. Ensure that the recording profile includes context-switch and ready-thread events if you need detailed scheduling information.

For the first experiment, compare worker activity across the original-handle-close phase and the later cooperative stop. CPU Usage (Precise) can show event waits and subsequent execution when the profile collects the necessary scheduler events. The successful API call and causal event acknowledgment are the direct evidence of post-close progress; a general scheduler trace does not promise to record every handle operation.

For the second experiment, compare the worker's exit with the later phase in which main queries the retained handle. A Thread Exit event records execution/lifecycle telemetry, not a complete object-reference-count history or proof of final deletion.

### PowerShell snapshots

For a coarse snapshot, replace `1234` with the printed process ID:

```powershell
$targetPid = 1234
$target = Get-Process -Id $targetPid
$target.Threads | Select-Object Id, ThreadState, TotalProcessorTime
$target.Refresh()
$target | Select-Object Id, HandleCount
```

Do not overwrite PowerShell's read-only `$PID` variable.

Thread collections can stop listing a worker after its execution ends, while a thread handle remains open. The process's total handle count does not identify which object a handle references and includes unrelated handles. These snapshots race with execution and process exit; they cannot establish which specific reference disappeared or whether final object deletion occurred.

Process Monitor may provide lifecycle context, but it does not show all scheduler states, every handle operation, or complete thread-origin provenance.

## Shared interpretation and limitations

### Thread IDs, handles, and execution

- **Thread ID (TID):** identifies a thread while that identifier is valid. TIDs can be reused and do not themselves retain a reference to the thread object.
- **Thread handle:** a process-local reference with access rights to the thread object. A valid handle supports operations allowed by its access rights and the object's state.
- **Thread execution:** the activity performed by the scheduled thread. Closing one handle is not a suspend request or a stop request.
- **Thread exit:** execution has ended, but valid handles may remain open and continue to refer to the thread object.

### Handle closure versus object deletion

Closing a handle releases the reference represented by that handle. It does not, by itself, prove that the kernel object has been deleted.

In the first experiment, the duplicate handle deliberately remains open after the original is closed. In the second, the worker has finished executing while main retains the handle for inspection. Both examples avoid claiming to observe the final reference disappearing or the exact instant of object deletion.

### Safety and reliability

Both programs operate on their own threads and use cooperative synchronization. Neither uses forced termination, arbitrary suspension, registry changes, or external-process actions.

Synchronization objects are not closed underneath a worker that may still be running. Worker state and deadlines remain valid for the required lifetime, and error paths request cooperative cleanup.

Timeouts and expected run durations are budgets rather than hard real-time guarantees. Starvation, debugger stops, and VM pauses can cause a reported failure.

## Learner questions

### Closing a handle while the worker continues

- Why is the event acknowledgment stronger than sleeping and hoping the counter increases?
- Why does duplicating a handle not create another executing thread?
- Which retained resource lets main join after the original handle closes?
- Why is a known TID not a substitute for owning a valid handle?
- What claim is ruled out by intentionally retaining the duplicate?

### Execution ends before the retained handle closes

- What proves execution ended, and what proves the object remains queryable?
- Why can a retained handle coexist with no selectable live worker in WinDbg?
- Why is the process's success exit code not the worker's return value `42`?
- What prevents this experiment from proving the instant of final object deletion?

## References

- [DuplicateHandle](https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-duplicatehandle)
- [CloseHandle](https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-closehandle)
- [Thread Handles and Identifiers](https://learn.microsoft.com/en-us/windows/win32/procthread/thread-handles-and-identifiers)
- [GetExitCodeThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getexitcodethread)
- [Terminating a Thread](https://learn.microsoft.com/en-us/windows/win32/procthread/terminating-a-thread)
- [`_beginthreadex`](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/beginthread-beginthreadex)
- [Windows Performance Recorder](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/windows-performance-recorder)
- [Windows Performance Analyzer](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/windows-performance-analyzer)

