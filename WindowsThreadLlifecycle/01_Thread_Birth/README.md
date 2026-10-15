# \# Windows Thread Creation: Immediate vs. Suspended Start

# 

# \## Overview

# 

# This lab compares two Windows thread creation strategies using the Win32 `CreateThread` API:

# 

# \- \*\*Immediate start\*\* (`immediate\_start.cpp`): the worker thread is created without `CREATE\_SUSPENDED` and is eligible to run as soon as the system permits.

# \- \*\*Suspended start\*\* (`suspended\_start.cpp`): the worker thread is created with `CREATE\_SUSPENDED` and cannot execute its application routine until `ResumeThread` removes its suspension.

# 

# The goal is to understand the difference between \*\*thread creation, thread execution, and scheduling\*\*, and to learn how to distinguish documented API guarantees from observations made during debugging.

# 

# Both examples use Win32 APIs in the worker thread and deliberately use `CreateThread`. In examples where the worker uses the C/C++ runtime, `\_beginthreadex` is generally more appropriate.

# 

# \## Learning Objectives

# 

# \- Understand what a successful `CreateThread` call guarantees.

# \- Distinguish thread existence from execution of its application routine.

# \- Observe thread scheduling and synchronization using Windows tools.

# \- Inspect thread IDs, stacks, and startup wrappers in WinDbg.

# \- Understand the limitations of snapshots, delays, and process-monitoring telemetry.

# \- Use synchronization primitives instead of assumptions about execution timing.

# 

# \## Requirements

# 

# \- Windows 10 version 1607 or later, or Windows 11.

# \- x64 architecture recommended.

# \- Visual Studio with the Windows SDK and C++ build tools.

# \- Process Explorer, WinDbg, and Process Monitor (Procmon) for optional inspection.

# 

# No third-party libraries are required.

# 

# \## Build and Run

# 

# Build each program separately from an x64 Native Tools Command Prompt in the directory containing the source files.

# 

# \### 1. Immediate Start

# 

# ```bat

# cl /nologo /std:c++17 /W4 /Od /Zi /EHsc immediate\_start.cpp /Fe:immediate\_start.exe /link /DEBUG

# immediate\_start.exe

# immediate\_start.exe --auto

# ```

# 

# \### 2. Suspended Start

# 

# ```bat

# cl /nologo /std:c++17 /W4 /Od /Zi /EHsc suspended\_start.cpp /Fe:suspended\_start.exe /link /DEBUG

# suspended\_start.exe

# suspended\_start.exe --auto

# ```

# 

# The `/Od` and `/Zi` options disable optimization and generate debugging information, making source-level and stack inspection easier.

# 

# Each program supports an interactive exercise and an automatic mode. Use the interactive mode when you want to inspect the thread at the checkpoints.

# 

# \---

# 

# \## Experiment 1: Immediate Thread Start

# 

# \*\*Source:\*\* `immediate\_start.cpp`

# 

# \### Question

# 

# Can the worker execute before the creator finishes the `CreateThread` call?

# 

# \*\*Yes.\*\* Without `CREATE\_SUSPENDED`, the new thread can execute before `CreateThread` returns to the calling thread. It can also begin executing later. A successful API call does not guarantee a particular scheduling order.

# 

# \### Expected Behavior

# 

# The program initializes shared handles and synchronization objects before creating the worker.

# 

# After creation, the main thread takes an atomic snapshot of the worker's entry flag. The snapshot may be either `0` or `1`.

# 

# \- `0` means the worker had not set the flag when the snapshot was taken.

# \- `1` means the worker had set the flag by the time of the snapshot.

# 

# However, a value of `1` does \*\*not\*\* prove that the worker entered its routine before `CreateThread` returned. Entry may have happened between the API return and the snapshot.

# 

# The worker also signals an event when it enters its routine. Waiting for that event establishes that entry has occurred by the time the wait completes.

# 

# This distinction illustrates the difference between an observation and a conclusion supported by that observation.

# 

# \### Exercise

# 

# 1\. Run `immediate\_start.exe`.

# 2\. Record the entry-flag snapshot, process ID (PID), and thread ID (TID).

# 3\. Repeat the experiment several times. You do not need to observe both possible snapshot values.

# 4\. Wait for the worker-generated entry event.

# 5\. In Process Explorer, locate the process using its printed PID and open \*\*Properties → Threads\*\*.

# 6\. Locate the worker using its printed TID and inspect its stack.

# 7\. Compare the stack with the suspended-start experiment after resumption.

# 8\. Press Enter to signal the stop event.

# 9\. Confirm that the main thread joins the worker and reads its exit code, `42`.

# 

# The worker remains available for inspection for up to 120 seconds after entry. No `ResumeThread` call is used in this example.

# 

# \---

# 

# \## Experiment 2: Explicitly Suspended Thread Start

# 

# \*\*Source:\*\* `suspended\_start.cpp`

# 

# \### Question

# 

# Does a successful thread creation call mean that the application's thread routine has executed?

# 

# \*\*No.\*\* With `CREATE\_SUSPENDED`, `CreateThread` returns a valid thread handle and TID, but the worker cannot reach `WorkerMain` until its suspend count reaches zero.

# 

# \### Expected Behavior

# 

# The program creates the worker using `CREATE\_SUSPENDED`.

# 

# At the first checkpoint:

# 

# \- The thread exists and has a valid handle and TID.

# \- The entry flag, `g\_workerEntered`, remains `0`.

# \- The worker has not entered `WorkerMain`.

# 

# Calling `ResumeThread` reduces the thread's suspend count. Its return value is the \*\*previous\*\* suspend count. A return value of `1` indicates that this call removed the last suspension.

# 

# The return from `ResumeThread` does not, by itself, prove that the worker has executed its application routine. The worker-generated entry event provides that evidence.

# 

# \### Exercise

# 

# 1\. Run `suspended\_start.exe`.

# 2\. Leave the first checkpoint open.

# 3\. Record the PID, TID, thread handle, and zero entry flag.

# 4\. Open Process Explorer and navigate to \*\*Properties → Threads\*\*.

# 5\. Locate the worker by its printed TID.

# 6\. Remember that seeing a thread in the list proves its existence, not that its application routine has executed.

# 7\. Press Enter to resume the worker.

# 8\. Wait for the entry event.

# 9\. At the second checkpoint, inspect the worker's stack.

# 10\. Press Enter to signal the stop event and join the worker.

# 11\. Confirm the worker's exit code.

# 

# The worker normally returns `42`. If inspection exceeds the 120-second safety timeout after entry, exit

