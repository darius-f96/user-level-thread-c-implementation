# User-Level Threads (Preemptive)

This project provides a preemptive user-level threading library using `getcontext`, `setcontext`, `makecontext`, `swapcontext`, and signal handling in Linux.

## Features
- Preemptive round-robin scheduler (SIGVTALRM + ITIMER_VIRTUAL)
- Thread API: create, join, exit, self, yield
- Mutex API: init, lock, unlock
- Deadlock detection report on SIGQUIT
- Read-write lock with writer preference

## Build

```bash
make
```

## Example

```bash
./example
```

## Notes
- Call `ult_init(quantum_us)` once before creating threads.
- SIGQUIT prints a deadlock report to stderr; the handler raises SIGVTALRM to force a preemptive scheduling point.
- Read-write locks follow writer preference: readers can enter only when no writer is active and there are no waiting writers.
