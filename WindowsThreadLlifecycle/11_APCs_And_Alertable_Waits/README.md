APCs and Alertable Waits on Windows

A hands-on Windows debugging lab demonstrating how ordinary user-mode Asynchronous Procedure Calls (APCs) behave when a target thread enters an alertable wait versus a non-alertable wait.

Table of Contents
Overview
Learning Objectives
Examples
Requirements
Build and Run
Experiment 1: Alertable Wait
Experiment 2: Non-Alertable Wait
Side-by-Side Comparison
Process Explorer and Procmon
WinDbg Investigation
Implementation and Safety Notes
Learner Questions
Official References
Overview

This lab compares two minimal C++ programs that queue an ordinary user APC using QueueUserAPC and demonstrate how the target thread's wait state affects callback delivery.

The examples share the same general structure: a main thread creates a worker, the worker signals that it has entered the controlled execution sequence, and main queues exactly one APC after observing that signal.

The intentional difference is the second argument to SleepEx:

TRUE: the worker enters an alertable wait and can execute the queued APC.
FALSE: the worker enters a non-alertable wait and does not execute the queued APC during that wait.

The experiments demonstrate that successful APC queuing does not guarantee callback execution. Delivery depends on the target thread reaching an appropriate delivery opportunity.

Learning Objectives

By completing this lab, you will learn how to:

Distinguish APC queuing from APC delivery and callback execution.
Compare alertable and non-alertable SleepEx behavior.
Verify which thread executes an APC callback.
Inspect thread state and call stacks using Process Explorer and WinDbg.
Understand why a successful QueueUserAPC return value is not proof of execution.
Recognize the limitations of coarse snapshots and CPU sampling when investigating short-lived callbacks.
Examples
File	Wait behavior	Expected callback count
alertable_wait.cpp	SleepEx(2000, TRUE)	1
non_alertable_wait.cpp	SleepEx(2000, FALSE)	0

Both programs use a benign, image-backed callback named BenignApc. The callback increments a counter using Interlocked operations and records its executing thread ID.

Neither example uses remote process injection, payloads, generated code, private executable memory, special user APCs, or kernel APC experiments.

Requirements
Windows 10 version 1607 or later, or Windows 11.
An x64 Windows SDK and C++ runtime.
Visual Studio or an x64 Native Tools Command Prompt.
WinDbg for callback and thread-context inspection.
Optional: Process Explorer, Process Monitor (Procmon), and Windows Performance Recorder/Analyzer (WPR/WPA).

The programs use CreateThread because the worker and callback make Win32 calls only. Console input and CRT output remain in the main thread.

Thread naming uses runtime resolution of SetThreadDescription in KernelBase.dll to support Windows 10 version 1607 compatibility.

No third-party libraries or shared training headers are required.

Build and Run

Run these commands from the directory containing the source files in an x64 Native Tools Command Prompt for Visual Studio.

Alertable example
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc alertable_wait.cpp /Fe:alertable_wait.exe /link /DEBUG
alertable_wait.exe
alertable_wait.exe --auto

Non-alertable example
cl /nologo /std:c++17 /W4 /Od /Zi /EHsc non_alertable_wait.cpp /Fe:non_alertable_wait.exe /link /DEBUG
non_alertable_wait.exe
non_alertable_wait.exe --auto

Visual Studio configuration

For either example:

Create an x64 Console App.
Disable precompiled headers and remove the generated source file.
Add only the selected example's .cpp file.
Enable C++17, /Od, and /Zi.
Enable linker /DEBUG and retain the matching PDB.
Keep the Console subsystem selected.

Build and debug each example independently.

Execution modes
Normal mode: pauses at a controlled observation gate so you can inspect the worker before releasing it.
Automatic mode (--auto): skips the input prompt and continues automatically.

Entry and join waits are each capped at six seconds. The automatic gate has a six-second limit, while normal-mode gate inspection is capped at 120 seconds. Under normal scheduling, both exercises fit within a 20-second execution budget.

These are timeout limits, not real-time guarantees. Debugger pauses and scheduler delays can affect execution.

Experiment 1: Alertable Wait

Source: alertable_wait.cpp

Purpose

Observe one ordinary user APC executing on an existing worker thread—not on the queuing main thread and not on a newly created callback thread.

The worker uses:

SleepEx(2000, TRUE);

Controlled sequence
Main names itself alertable-main and creates a worker named alertable-target.
The program prints the process ID (PID) and thread IDs (TIDs) in decimal and hexadecimal.
The worker signals actual entry before waiting on a non-alertable release gate.
Main observes the entry event and queues exactly one APC using QueueUserAPC.
Main checks that the callback count is still zero while the gate remains closed.
In normal mode, the program pauses so you can inspect the target thread and set a debugger breakpoint.
Press Enter to release the gate.
The worker enters SleepEx(2000, TRUE), dispatches the pending callback, and returns from the wait.
The worker exits, and main joins it before reading the final results or releasing the context and handles.

Waiting for actual worker entry before queuing the APC excludes the documented special timing case in which an APC queued before a thread starts can be delivered when that thread begins execution.

Expected results
Observation	Expected value
APCs queued	1
Callback count at the closed gate	0
Final callback count	1
SleepEx return value	WAIT_IO_COMPLETION (0x000000c0)
Callback TID	Equal to target TID
Callback TID versus main TID	Different
Process exit code	0 on success

The callback records its executing TID and increments the counter using Interlocked operations.

Important timing detail

A pending APC can execute as soon as the worker enters an alertable wait. Therefore, the requested two-second sleep may return almost immediately.

WAIT_IO_COMPLETION indicates that the alertable wait was interrupted by APC or I/O completion; it does not mean the requested delay expired. The program does not assert an exact elapsed time.

Experiment 2: Non-Alertable Wait

Source: non_alertable_wait.cpp

Purpose

Demonstrate that a successful QueueUserAPC call does not prove that the callback executed.

The worker uses:

SleepEx(2000, FALSE);


It does not enter another alertable wait before exiting.

Controlled sequence
Main names itself nonalertable-main and creates a worker named nonalertable-target.
The program prints the PID and thread IDs in decimal and hexadecimal.
The worker records its TID, signals the entry event, and waits on the non-alertable release gate.
Main observes the entry event and queues exactly one APC.
Main confirms that the callback count remains zero while the gate is closed.
In normal mode, the program pauses so you can inspect the worker.
Press Enter to open the gate.
The worker calls SleepEx(2000, FALSE), records the return value, and exits without another wait.
Main joins the worker before reading the final record or releasing the context and handles.
Expected results
Observation	Expected value
APCs queued	1
Callback count at the closed gate	0
Final callback count	0
SleepEx return value	0
Callback TID	0 (not recorded)
Worker exit code	0 on success

The pending APC is discarded when the worker thread exits. Main does not cancel the APC.

A callback TID of zero means that no callback execution was recorded; it is not the worker's actual TID.

The expected successful process exit code is 0. API failures or violated program invariants result in exit code 1.

Important interpretation

The result does not mean QueueUserAPC failed. The APC was queued successfully, but the worker never reached an alertable wait before terminating.

An ordinary user APC does not execute merely because its target thread is sleeping.

Side-by-Side Comparison

The two experiments differ in one intentional delivery-rule change: the Boolean argument to SleepEx.

Property	Alertable example	Non-alertable example
Source	alertable_wait.cpp	non_alertable_wait.cpp
Wait call	SleepEx(2000, TRUE)	SleepEx(2000, FALSE)
APC queued	One	One
Worker entered before queuing	Yes	Yes
Callback executes	Yes	No
Final callback count	1	0
Wait return value	WAIT_IO_COMPLETION	0
Callback TID	Target thread's TID	0, not recorded
Pending APC after worker exits	Already delivered	Discarded on thread exit
Core conclusion

Queuing is not delivery, and delivery is not established by queuing success alone.

The alertable example establishes execution using the callback counter, the recorded callback TID, the wait return value, and a debugger breakpoint. The non-alertable example demonstrates the contrasting outcome: the queued callback is not executed before the worker exits.

Process Explorer and Procmon
Process Explorer

Use the PID and TIDs printed by the program to locate the target worker.

Open the process properties and select the Threads tab.
Identify the worker by its printed decimal TID.
Inspect its stack while the program is paused at the release gate.

At the gate, the worker should be approaching or inside the event wait. After release, it may move into SleepEx and exit.

For the alertable example, do not expect a later GUI refresh to capture BenignApc. The callback is short-lived and can finish before the next refresh. A breakpoint inside the callback provides more direct evidence.

Process Monitor (Procmon)

Capture process and thread activity starting from process startup.

Procmon can show the worker's original creation and eventual exit. APC queuing and delivery do not create a separate thread just for the callback.

Keep these limitations in mind:

The worker is created by CreateThread, not by QueueUserAPC.
APC callbacks execute within an existing target thread when delivered.
Other unrelated process threads may exist.
Procmon thread events are not a complete APC-delivery trace.
The absence of a new thread-creation event does not establish whether a callback executed.

Use the program's recorded results and WinDbg to establish callback execution.

WinDbg Investigation

Use x64 WinDbg to inspect the thread context and compare the two examples.

Replace the symbol path below with the actual directory containing your training sources if necessary.

1. Alertable example: stop inside the callback

Launch alertable_wait.exe under WinDbg in normal mode.

.symfix
.sympath+ C:\training\11_APCs_And_Alertable_Waits
.reload
x alertable_wait!*BenignApc*
bu alertable_wait!BenignApc
g


At the queued-but-gated console prompt, you can break into the debugger and inspect the worker before releasing the gate:

~
~~[HEX_TID]s
k
g


Replace HEX_TID with the printed target TID in hexadecimal, without the 0x prefix.

Important: WinDbg thread indexes are not operating-system thread IDs. For example, ~2s selects debugger thread index 2, whereas ~~[HEX_TID]s selects the operating-system thread with the specified TID.

Continue the debugger with g, then press Enter in the console to release the gate.

When the debugger stops inside BenignApc, inspect the executing context:

~.
k
r rip
ln @rip
lm
!address @rip
dv /t
g


Expected evidence:

The current OS TID is the worker's TID.
The instruction pointer resolves to the callback's image-backed code.
The stack may include an APC dispatcher such as ntdll!KiUserApcDispatcher, wait-related frames, and ApcTarget.

Exact stack frames and unwind visibility vary by Windows build and available symbols.

If you also want to inspect SleepEx, you can set a breakpoint on KERNELBASE!SleepEx. At x64 function entry, ECX should contain 2000 and EDX should contain 1:

bu KERNELBASE!SleepEx
g
r ecx edx
k


The callback breakpoint is the reliable point at which to inspect callback execution; a later GUI snapshot may miss it.

2. Non-alertable example: verify that the callback breakpoint does not fire

Launch non_alertable_wait.exe under WinDbg in normal mode.

.symfix
.sympath+ C:\training\11_APCs_And_Alertable_Waits
.reload
x non_alertable_wait!*BenignApc*
bu non_alertable_wait!BenignApc
g


At the queued-but-gated console prompt, break into the debugger:

~
~~[HEX_TID]s
k
r rip
ln @rip
bu KERNELBASE!SleepEx
g


Replace HEX_TID with the worker's printed hexadecimal TID, without the 0x prefix.

Continue the debugger before pressing Enter in the console.

At the SleepEx breakpoint, inspect the x64 argument registers and stack:

r ecx edx
k
g


At function entry:

ECX should contain 2000 (0x7d0).
EDX should contain 0, representing FALSE.

The BenignApc breakpoint should not be reached during this run because the worker never enters an alertable wait.

If the Windows export resolves through a different symbol path, use:

x KERNELBASE!*SleepEx*


Then set the breakpoint on the resolved function.

Debugger breaks perturb timing. A paused or stale run can exceed the gate timeout, so restart the exercise if necessary.

Implementation and Safety Notes
Ordinary user APC delivery

These examples concern ordinary user APCs queued with QueueUserAPC. They do not demonstrate kernel APCs or the newer special user APC mechanisms, which have different delivery rules.

A successful queue operation establishes that queuing succeeded; it does not independently establish callback execution.

Thread and callback lifetime

The worker and callback make Win32 calls only. Main owns CRT console output and input.

Both examples join the worker before reading the final results or releasing the associated context and handles. The callback does not recursively enter another alertable wait.

In production code, callbacks executing in an existing thread context require careful consideration of reentrancy, object lifetimes, synchronization, and which waits permit delivery.

Timing and observation limits
The requested two-second sleep is not an exact elapsed-time guarantee in the alertable example.
Scheduler delays and debugger pauses can invalidate timing expectations.
Optional WPR/WPA CPU sampling may miss the entire callback because its execution can be extremely brief.
Missing CPU samples do not establish non-delivery.
Seeing image-backed instructions establishes code provenance, not a general behavioral verdict.
User-mode WinDbg commands such as k, ~, and !address do not directly expose kernel ETHREAD or KTHREAD objects.

For these exercises, the joined counter, recorded TID, wait return value, and callback breakpoint provide more useful evidence than a coarse stack snapshot alone.

Learner Questions

Use these questions to check your understanding.

Why does the callback count remain zero while the release gate is closed in both examples?
Why can the alertable two-second sleep return almost immediately?
What does a successful QueueUserAPC call prove, and what does it not prove?
Why must main observe actual worker entry before queuing the APC?
Which observations establish that the alertable callback ran on the target thread rather than on main?
Why is a callback breakpoint stronger evidence than a Process Explorer refresh?
What happens to the pending APC when the non-alertable worker exits?
Why does the absence of a new thread in Procmon tell you little about APC delivery?
How do the SleepEx return values differ between the two experiments?
How do ordinary user APCs differ from special user APCs and kernel APCs?
Official References
QueueUserAPC
SleepEx
Asynchronous Procedure Calls
InterlockedIncrement
SetThreadDescription
Alertable I/O
WinDbg thread syntax
x64 calling convention

Lab summary: These two programs isolate one important Windows threading behavior: an ordinary user APC queued to a worker can execute when that worker enters an alertable wait, but it is not delivered merely because the worker is sleeping. Compare the callback count, recorded TID, wait result, and debugger evidence to understand the difference between queuing and execution.
