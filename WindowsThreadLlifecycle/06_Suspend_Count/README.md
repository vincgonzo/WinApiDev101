# Two suspension counts, two resumes

Source: [suspend_count.cpp](suspend_count.cpp). Shared preparation:
[TOOLS.md](../TOOLS.md) and [training overview](../README.md).

## Objective

Show that `ResumeThread` removes one suspension count and reports the **previous**
count. Removing one of two counts does not run the application routine. Removing
the last count makes execution possible, but a separate event is needed to prove
that the application routine actually ran.

## Standalone build and run

Use Windows 10 1607+ / Windows 11 x64, an x64 Native Tools Command Prompt and the
Windows SDK. Compile this translation unit alone; it has its own `main`:

```bat
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc suspend_count.cpp /Fe:suspend_count.exe /link /DEBUG
suspend_count.exe
suspend_count.exe --auto
```

Keep the PDB beside the executable. No optimization plus `/Zi /link /DEBUG` is
important for source/stack inspection. `CreateThread` is intentional: the worker
only uses Win32 APIs and Interlocked operations, with no CRT calls. Output and
console handling happen in main. The shared entry/error flags use Interlocked
access on both sides; `volatile` alone is not used as synchronization.

Exit **0** means the count, event, flag, exit-code and cleanup checks succeeded;
**1** indicates an API/timeout/invariant failure. This exercise has no optional
capability skip on the supported Windows baseline. Cross-compilation is not a
Windows runtime test.

## Phases and expected evidence

1. Main prints PID/TIDs in decimal and hexadecimal plus thread/event handles. It
   creates the worker with `CREATE_SUSPENDED`, which supplies the first count.
2. Before any application entry or resume, main calls `SuspendThread` exactly
   once. Its return must be **1**, producing a count of two. This is not an
   invitation to suspend a running thread; only this never-run child is targeted.
3. At phase 1, inspect the never-entered worker. Enter, or the two-second checkpoint
   deadline, calls the first `ResumeThread`. It must return **2**.
4. Main performs a **250 ms**, nonzero entry-event wait, expecting `WAIT_TIMEOUT`,
   then reads the Interlocked entry flag, expecting zero. The known remaining
   suspend count plus these observations distinguishes this from an arbitrary
   scheduling delay. The API count result is the key mechanism, not silence alone.
5. Phase 2 is a second checkpoint before the last resume. That resume must return
   **1**. Main waits for the entry event and checks flag one, proving real entry.
6. In phase 3 the worker has entered and is approaching or inside its cooperative
   stop-event wait. It is no longer suspended. Main signals stop, joins the worker,
   verifies return **42**, and closes the handles.

Each interactive checkpoint lasts at most two seconds; Enter advances it. No
console/redirected input advances immediately. `--auto` bypasses every prompt and
normally completes in about 250 ms. Interactive execution normally takes about
6.25 seconds. Waits share an 18-second safety deadline, and cleanup undoes only
the suspension counts introduced by this program. On failure, worker state stays
in process-lifetime storage; synchronization handles are not closed underneath a
worker that failed to join. Debugger/VM pauses or OS starvation invalidate strict
wall-clock expectations. A run stopped too long may report failure; rerun it.

## Process Explorer

Configure symbols before launch as described in [TOOLS.md](../TOOLS.md). In the
printed process's **Properties > Threads**, find `suspend-count-worker` by the
decimal TID. Main is `suspend-count-main`. Compare the first two phases with phase
3 using **Stack**, CPU deltas, and any available state display. Tool/UI versions
do not all expose suspend count directly. Do not use Process Explorer's Suspend
or Resume actions: they would change the experiment's count accounting.

The existence of a listed thread does not establish application entry. A stack
seen after the entry event may still be approaching the event wait; the event
establishes entry, not an exact kernel wait state at a later snapshot.

## WinDbg

Launch in x64 WinDbg. Set symbols and an application entry breakpoint:

```text
.symfix C:\Symbols
.sympath+ C:\labs\06_Suspend_Count
.reload /f
bu suspend_count!WorkerMain
g
```

The breakpoint should not be hit after the first resume. It becomes reachable
only after the final count is removed. At an inspection break:

```text
~
~~[HEX_TID]s
kn
!teb
!handle PRINTED_THREAD_HANDLE f
!handle PRINTED_ENTRY_EVENT_HANDLE f
dd suspend_count!g_workerEntered L1
bc *
g
```

Substitute the actual values. `~~[HEX_TID]s` selects an OS TID using its hex digits
without `0x`; `~2s` selects debugger index 2, which is a different namespace.
Debugger thread-freeze/suspension mechanisms can perturb apparent counts. Do not
issue debugger suspend/resume commands or edit the flag. Debugger breaks stop the
process and change timing; a long break can consume the safety budget before
actual entry. User-mode `!teb` does not expose the kernel ETHREAD/KTHREAD. Optional
kernel inspection belongs in a dedicated debugging VM; see the shared tools guide.

## WPR/WPA and PowerShell

For uninterrupted lifecycle/scheduling evidence, install Windows Performance
Toolkit and use an elevated recording console:

```bat
wpr -status
wpr -start GeneralProfile -filemode
suspend_count.exe
wpr -stop "%TEMP%\suspend-count.etl"
```

Do not interrupt another user's recording. In WPA, filter the process and TID in
thread-lifetime and **CPU Usage (Precise)** views. A profile must include context
switches and ready-thread events for the Precise view. The API count/flag/event
checks are the primary evidence of the application-entry claim; a lifecycle event
alone cannot prove the application's first instruction ran.

A coarse second-console PowerShell snapshot is:

```powershell
$targetPid = 1234
(Get-Process -Id $targetPid).Threads | ForEach-Object {
    $reason = if ($_.ThreadState -eq 'Wait') { $_.WaitReason } else { $null }
    [pscustomobject]@{ TID = $_.Id; State = $_.ThreadState; WaitReason = $reason }
}
```

Replace `1234`; never overwrite PowerShell's `$PID`. Queries race thread exit and
are not a suspend-count oracle. Procmon can capture lifecycle events, not the full
scheduler-state timeline or all thread-origin provenance.

## Analyst relevance and limits

Suspension is a count-based execution restriction, not the same thing as waiting
on an application event and not an additional state in the same dispatcher-state
enum. A waiting worker can become runnable when its event is signaled; a remaining
suspend count must separately be removed. Never suspend arbitrary running threads:
they can hold CRT, allocator, loader, or application locks and deadlock the very
inspection/control path you need. This program never uses forced termination,
cross-process handles, registry changes, or unbounded work.

## Learner questions

- Why does a return value of two mean the worker is still suspended afterward?
- What proves actual entry, rather than merely eligibility to execute?
- How does phase 3's event wait differ from phase 2's remaining suspension?
- Why would a debugger's own suspension actions invalidate this experiment?

References: [SuspendThread](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-suspendthread),
[ResumeThread](https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-resumethread),
[suspending thread execution](https://learn.microsoft.com/windows/win32/procthread/suspending-thread-execution).
