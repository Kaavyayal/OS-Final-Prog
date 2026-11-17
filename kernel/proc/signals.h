#pragma once
#include "proc/thread.h"
#include "sched/sync.h"
namespace Signals {
bool pending(Thread);
sync<void> deliver(Thread);
void register_process(Process);
Process find(int);
bool exists(int);
void reap(int);
void fault(Thread, uint64_t, int code = 1);
} // namespace Signals
namespace Futex {
void cancel(Process);
void wake(Process, uint64_t, int);
} // namespace Futex
