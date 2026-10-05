# Process Scheduler

## What does it do
Simulated process scheduling system for my CS205 Operating Systems project. It runs a non-preemptive, FCFS algorithm with different priority jobs. Allows at most three concurrently running processes, and always chooses the next highest priority job in the ready queue once there is a free slot.

**Built with:** C, POSIX process management, linked lists

## Highlights
- Interactive shell (`cs205$`) to run, stop, resume, kill and list background processes
- Priority scheduling (P1 is highest), with FCFS to break ties
- Non-preemptive, so a running job is never kicked out by a higher priority arrival
- Simulated events that block a process mid-run until the user triggers them
- All queues (ready, running, blocked) are linked lists, no arrays

## Process States
| Code | State | Description |
| ---- | ----- | --- |
|0|Running|Currently executing (max 3 at a time)|
|1|Ready|Eligible to run, waiting for a CPU slot|
|2|Stopped|Suspended by the user|
|3|Terminated|Completed or killed, cannot be resumed|
|4|Blocked|Waiting for a predefined event|

> `prog` is the test program. It takes a `file_name` and a number `n`, and writes to the file every second for `n` seconds.

## Commands
- `run [program] [file_name] [n] [priority] [event@cycle]`: Start a program with a priority (e.g. `P2`). Optionally add an event like `E1@4` to block it after 4 seconds of running until `E1` is triggered
    - `[file_name]` and `[n]` are the params for the test program (See above).
- `stop [PID]`: Suspend a running process and dispatch the next highest priority ready job
- `resume [PID]`: Move a stopped process back to ready. It doesn't preempt anything, just waits for a free slot
- `kill [PID]`: Terminate a process and dispatch the next ready job if a slot frees up
- `event [E1/E2/E3]`: Trigger an event and move any processes waiting on it back to ready (FCFS if there's more than one)
- `list`: Show PID, state and priority of every process
- `exit`: Kill any remaining child processes and quit

## Predefined events
These are meant to simulate I/O or other blocking events that occur in the course of real-life execution of processes. These are triggered using `event [E1/E2/E3]`.

| Event | Meaning |
| ----- | ------- |
|E1|Keyboard input|
|E2|Disk I/O completed|
|E3|Network data available|

## How to run
Needs a Linux/Unix environment (WSL works fine).
```sh
gcc -o manager manager.c
gcc -o out/prog tools/prog.c
./manager
```

## Example
```
cs205$ run ./prog x40 40 P3
cs205$ run ./prog x50 50 P1
cs205$ run ./prog x60 60 P4 E1@40
cs205$ run ./prog x70 70 P2
cs205$ list
PID     STATE   PRIORITY
11001   0       P3
11002   0       P1
11003   0       P4
-       1       P2
```
Only three can run at once, so P2 waits in the ready queue even though it's higher priority than P3 and P4. Once P3 finishes, P2 gets the free slot. When P4 hits 40 seconds it blocks on `E1` until you run `event E1`.
