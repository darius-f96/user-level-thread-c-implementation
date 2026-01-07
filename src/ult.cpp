#include "ult.h"

#include <csignal>
#include <cstring>
#include <deque>
#include <string>
#include <map>
#include <set>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

namespace {

constexpr size_t kStackSize = 64 * 1024;
constexpr int kMainTid = 0;

enum class ThreadState {
  RUNNING,
  READY,
  BLOCKED,
  TERMINATED,
};

enum class WaitType {
  NONE,
  MUTEX,
  RWLOCK,
  JOIN,
};

struct Thread {
  ult_tid_t tid = -1;
  ucontext_t ctx{};
  ThreadState state = ThreadState::READY;
  void* retval = nullptr;
  void* stack = nullptr;
  std::vector<ult_tid_t> joiners;
  WaitType wait_type = WaitType::NONE;
  void* wait_obj = nullptr;
  bool in_ready_queue = false;
  uintptr_t stack_low = 0;
  uintptr_t stack_high = 0;
};

std::map<ult_tid_t, Thread> g_threads;
std::deque<ult_tid_t> g_ready_queue;
std::deque<ult_tid_t> g_zombie_queue;
ult_tid_t g_current_tid = kMainTid;
int g_next_tid = 1;
bool g_initialized = false;
volatile sig_atomic_t g_report_deadlock = 0;
volatile sig_atomic_t g_need_resched = 0;
volatile sig_atomic_t g_in_preempt = 0;
volatile uintptr_t g_stack_low = 0;
volatile uintptr_t g_stack_high = 0;

sigset_t g_timer_mask;

class SignalBlocker {
 public:
  SignalBlocker() { sigprocmask(SIG_BLOCK, &g_timer_mask, &old_mask_); }
  ~SignalBlocker() { sigprocmask(SIG_SETMASK, &old_mask_, nullptr); }

 private:
  sigset_t old_mask_{};
};

void enqueue_ready(ult_tid_t tid) {
  if (tid < 0) {
    return;
  }
  auto it = g_threads.find(tid);
  if (it == g_threads.end()) {
    return;
  }
  Thread& t = it->second;
  if (t.state == ThreadState::READY && !t.in_ready_queue) {
    t.in_ready_queue = true;
    g_ready_queue.push_back(tid);
  }
}

void reap_zombies() {
  while (!g_zombie_queue.empty()) {
    ult_tid_t tid = g_zombie_queue.front();
    g_zombie_queue.pop_front();
    auto it = g_threads.find(tid);
    if (it == g_threads.end()) {
      continue;
    }
    Thread& t = it->second;
    if (t.stack) {
      delete[] reinterpret_cast<char*>(t.stack);
      t.stack = nullptr;
    }
  }
}

void schedule_next();

void thread_trampoline(uintptr_t fn_ptr, uintptr_t arg_ptr) {
  auto fn = reinterpret_cast<void* (*)(void*)>(fn_ptr);
  void* arg = reinterpret_cast<void*>(arg_ptr);
  void* ret = fn(arg);
  ult_exit(ret);
}

void report_deadlocks() {
  g_report_deadlock = 0;

  std::map<ult_tid_t, ult_tid_t> wait_for;
  for (const auto& [tid, t] : g_threads) {
    if (t.state != ThreadState::BLOCKED) {
      continue;
    }
    if (t.wait_type == WaitType::MUTEX) {
      auto* mtx = reinterpret_cast<ult_mutex*>(t.wait_obj);
      if (mtx && mtx->locked) {
        wait_for[tid] = mtx->owner;
      }
    } else if (t.wait_type == WaitType::JOIN) {
      auto* target = reinterpret_cast<ult_tid_t*>(t.wait_obj);
      if (target) {
        wait_for[tid] = *target;
      }
    } else if (t.wait_type == WaitType::RWLOCK) {
      auto* lock = reinterpret_cast<ult_rwlock*>(t.wait_obj);
      if (lock && lock->writer_active && lock->writer >= 0) {
        wait_for[tid] = lock->writer;
      } else if (lock && !lock->writer_waiters.empty()) {
        wait_for[tid] = lock->writer_waiters.front();
      }
    }
  }

  std::set<ult_tid_t> reported;
  std::string out = "Deadlock report (SIGQUIT):\n";
  for (const auto& [start, _] : wait_for) {
    if (reported.count(start)) {
      continue;
    }
    std::set<ult_tid_t> seen;
    ult_tid_t cur = start;
    while (wait_for.count(cur)) {
      if (seen.count(cur)) {
        out += "  cycle detected starting at tid " + std::to_string(cur) + "\n";
        for (auto tid : seen) {
          reported.insert(tid);
        }
        break;
      }
      seen.insert(cur);
      cur = wait_for[cur];
    }
  }

  if (out == "Deadlock report (SIGQUIT):\n") {
    out += "  no cycles detected\n";
  }

  write(STDERR_FILENO, out.c_str(), out.size());
}

void maybe_report_deadlocks() {
  if (g_report_deadlock) {
    report_deadlocks();
  }
}

void check_preempt() {
  if (!g_need_resched) {
    return;
  }
  g_need_resched = 0;
  schedule_next();
}

void preempt_trampoline() {
  g_in_preempt = 1;
  check_preempt();
  g_in_preempt = 0;
}

void sigvtalrm_handler(int, siginfo_t*, void* uctx) {
  g_need_resched = 1;
#if defined(__x86_64__)
  if (g_in_preempt) {
    return;
  }
  auto* ctx = reinterpret_cast<ucontext_t*>(uctx);
  greg_t rip = ctx->uc_mcontext.gregs[REG_RIP];
  if (rip == reinterpret_cast<greg_t>(preempt_trampoline)) {
    return;
  }
  greg_t rsp = ctx->uc_mcontext.gregs[REG_RSP];
  uintptr_t stack_low = g_stack_low;
  uintptr_t stack_high = g_stack_high;
  if (stack_low == 0 || stack_high == 0) {
    return;
  }
  uintptr_t new_rsp = static_cast<uintptr_t>(rsp) - sizeof(uintptr_t);
  if (new_rsp < stack_low || new_rsp + sizeof(uintptr_t) > stack_high) {
    return;
  }
  auto* stack = reinterpret_cast<uintptr_t*>(rsp);
  stack -= 1;
  *stack = static_cast<uintptr_t>(rip);
  ctx->uc_mcontext.gregs[REG_RSP] = reinterpret_cast<greg_t>(stack);
  ctx->uc_mcontext.gregs[REG_RIP] = reinterpret_cast<greg_t>(preempt_trampoline);
#else
  (void)uctx;
#endif
}

void sigquit_handler(int) {
  g_report_deadlock = 1;
  g_need_resched = 1;
}

void init_signals() {
  sigemptyset(&g_timer_mask);
  sigaddset(&g_timer_mask, SIGVTALRM);

  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = sigvtalrm_handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGVTALRM, &sa, nullptr);

  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sigquit_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGQUIT, &sa, nullptr);

  signal(SIGPIPE, SIG_IGN);
}

void start_timer(unsigned int quantum_us) {
  itimerval tv{};
  tv.it_interval.tv_sec = quantum_us / 1000000;
  tv.it_interval.tv_usec = quantum_us % 1000000;
  tv.it_value = tv.it_interval;
  setitimer(ITIMER_VIRTUAL, &tv, nullptr);
}

void schedule_next() {
  SignalBlocker guard;
  maybe_report_deadlocks();
  reap_zombies();

  if (g_ready_queue.empty()) {
    return;
  }

  ult_tid_t prev_tid = g_current_tid;
  Thread& prev = g_threads[prev_tid];
  if (prev.state == ThreadState::RUNNING) {
    prev.state = ThreadState::READY;
    enqueue_ready(prev_tid);
  }

  ult_tid_t next_tid = g_ready_queue.front();
  g_ready_queue.pop_front();
  Thread& next = g_threads[next_tid];
  next.in_ready_queue = false;
  next.state = ThreadState::RUNNING;
  g_current_tid = next_tid;
  g_stack_low = next.stack_low;
  g_stack_high = next.stack_high;

  if (prev_tid == next_tid) {
    return;
  }

  swapcontext(&prev.ctx, &next.ctx);
  g_stack_low = prev.stack_low;
  g_stack_high = prev.stack_high;
}

void ensure_initialized() {
  if (g_initialized) {
    return;
  }
  ult_init(5000);
}

void wake_joiners(Thread& t) {
  for (ult_tid_t tid : t.joiners) {
    Thread& waiter = g_threads[tid];
    if (waiter.state == ThreadState::BLOCKED) {
      waiter.state = ThreadState::READY;
      waiter.wait_type = WaitType::NONE;
      waiter.wait_obj = nullptr;
      enqueue_ready(tid);
    }
  }
  t.joiners.clear();
}

void maybe_reap_thread(ult_tid_t tid) {
  auto it = g_threads.find(tid);
  if (it == g_threads.end()) {
    return;
  }
  Thread& t = it->second;
  if (t.state != ThreadState::TERMINATED || !t.stack) {
    return;
  }
  delete[] reinterpret_cast<char*>(t.stack);
  t.stack = nullptr;
  if (t.joiners.empty()) {
    g_threads.erase(it);
  }
}

}  // namespace

int ult_init(unsigned int quantum_us) {
  if (g_initialized) {
    return 0;
  }

  g_initialized = true;
  init_signals();
  start_timer(quantum_us);

  Thread main_thread;
  main_thread.tid = kMainTid;
  main_thread.state = ThreadState::RUNNING;
  getcontext(&main_thread.ctx);
  main_thread.stack_low = 0;
  main_thread.stack_high = 0;
  g_threads[kMainTid] = main_thread;
  g_current_tid = kMainTid;
  g_stack_low = 0;
  g_stack_high = 0;

  return 0;
}

int ult_create(ult_tid_t* tid, void* (*start_routine)(void*), void* arg) {
  ensure_initialized();
  if (!tid || !start_routine) {
    return -1;
  }
  check_preempt();

  SignalBlocker guard;
  Thread t;
  t.tid = g_next_tid++;
  t.state = ThreadState::READY;
  t.stack = new char[kStackSize];
  t.stack_low = reinterpret_cast<uintptr_t>(t.stack);
  t.stack_high = t.stack_low + kStackSize;

  getcontext(&t.ctx);
  t.ctx.uc_stack.ss_sp = t.stack;
  t.ctx.uc_stack.ss_size = kStackSize;
  t.ctx.uc_link = nullptr;
  sigemptyset(&t.ctx.uc_sigmask);
  makecontext(&t.ctx, reinterpret_cast<void (*)()>(thread_trampoline), 2,
              reinterpret_cast<uintptr_t>(start_routine),
              reinterpret_cast<uintptr_t>(arg));

  g_threads[t.tid] = t;
  enqueue_ready(t.tid);
  *tid = t.tid;
  return 0;
}

ult_tid_t ult_self() {
  ensure_initialized();
  check_preempt();
  return g_current_tid;
}

int ult_join(ult_tid_t tid, void** retval) {
  ensure_initialized();
  if (tid == g_current_tid) {
    return -1;
  }
  check_preempt();

  SignalBlocker guard;
  auto it = g_threads.find(tid);
  if (it == g_threads.end()) {
    return -1;
  }

  Thread& target = it->second;
  if (target.state == ThreadState::TERMINATED) {
    if (retval) {
      *retval = target.retval;
    }
    maybe_reap_thread(tid);
    return 0;
  }

  Thread& self = g_threads[g_current_tid];
  self.state = ThreadState::BLOCKED;
  self.wait_type = WaitType::JOIN;
  self.wait_obj = &target.tid;
  target.joiners.push_back(self.tid);

  schedule_next();

  if (retval) {
    *retval = target.retval;
  }

  maybe_reap_thread(tid);
  return 0;
}

void ult_exit(void* retval) {
  ensure_initialized();
  SignalBlocker guard;
  Thread& self = g_threads[g_current_tid];
  self.retval = retval;
  self.state = ThreadState::TERMINATED;

  wake_joiners(self);

  if (g_ready_queue.empty()) {
    auto it = g_threads.find(kMainTid);
    if (it != g_threads.end() && it->second.state != ThreadState::TERMINATED &&
        kMainTid != g_current_tid) {
      Thread& main_thread = it->second;
      main_thread.state = ThreadState::RUNNING;
      main_thread.in_ready_queue = false;
      g_current_tid = kMainTid;
      g_stack_low = main_thread.stack_low;
      g_stack_high = main_thread.stack_high;
      setcontext(&main_thread.ctx);
    }
    _exit(0);
  }

  if (self.stack) {
    g_zombie_queue.push_back(self.tid);
  }

  ult_tid_t next_tid = g_ready_queue.front();
  g_ready_queue.pop_front();
  Thread& next = g_threads[next_tid];
  next.in_ready_queue = false;
  next.state = ThreadState::RUNNING;
  g_current_tid = next_tid;
  g_stack_low = next.stack_low;
  g_stack_high = next.stack_high;
  setcontext(&next.ctx);
  _exit(0);
}

int ult_yield() {
  ensure_initialized();
  check_preempt();
  schedule_next();
  return 0;
}

int ult_mutex_init(ult_mutex* mtx) {
  if (!mtx) {
    return -1;
  }
  mtx->initialized = true;
  mtx->locked = false;
  mtx->owner = -1;
  mtx->waiters.clear();
  return 0;
}

int ult_mutex_lock(ult_mutex* mtx) {
  ensure_initialized();
  if (!mtx || !mtx->initialized) {
    return -1;
  }
  check_preempt();

  SignalBlocker guard;
  if (!mtx->locked) {
    mtx->locked = true;
    mtx->owner = g_current_tid;
    return 0;
  }

  if (mtx->owner == g_current_tid) {
    return -1;
  }

  Thread& self = g_threads[g_current_tid];
  self.state = ThreadState::BLOCKED;
  self.wait_type = WaitType::MUTEX;
  self.wait_obj = mtx;
  mtx->waiters.push_back(self.tid);

  schedule_next();
  return 0;
}

int ult_mutex_unlock(ult_mutex* mtx) {
  ensure_initialized();
  if (!mtx || !mtx->initialized || !mtx->locked) {
    return -1;
  }
  check_preempt();

  SignalBlocker guard;
  if (mtx->owner != g_current_tid) {
    return -1;
  }

  if (!mtx->waiters.empty()) {
    ult_tid_t next = mtx->waiters.front();
    mtx->waiters.pop_front();
    mtx->owner = next;

    Thread& waiter = g_threads[next];
    waiter.state = ThreadState::READY;
    waiter.wait_type = WaitType::NONE;
    waiter.wait_obj = nullptr;
    enqueue_ready(next);
  } else {
    mtx->locked = false;
    mtx->owner = -1;
  }

  return 0;
}

int ult_rwlock_init(ult_rwlock* lock) {
  if (!lock) {
    return -1;
  }
  lock->initialized = true;
  lock->writer_active = false;
  lock->writer = -1;
  lock->readers = 0;
  lock->waiting_writers = 0;
  lock->reader_waiters.clear();
  lock->writer_waiters.clear();
  return 0;
}

int ult_rwlock_rdlock(ult_rwlock* lock) {
  ensure_initialized();
  if (!lock || !lock->initialized) {
    return -1;
  }
  check_preempt();

  SignalBlocker guard;
  if (!lock->writer_active && lock->waiting_writers == 0) {
    lock->readers++;
    return 0;
  }

  Thread& self = g_threads[g_current_tid];
  self.state = ThreadState::BLOCKED;
  self.wait_type = WaitType::RWLOCK;
  self.wait_obj = lock;
  lock->reader_waiters.push_back(self.tid);

  schedule_next();
  return 0;
}

int ult_rwlock_wrlock(ult_rwlock* lock) {
  ensure_initialized();
  if (!lock || !lock->initialized) {
    return -1;
  }
  check_preempt();

  SignalBlocker guard;
  if (!lock->writer_active && lock->readers == 0) {
    lock->writer_active = true;
    lock->writer = g_current_tid;
    return 0;
  }

  lock->waiting_writers++;
  Thread& self = g_threads[g_current_tid];
  self.state = ThreadState::BLOCKED;
  self.wait_type = WaitType::RWLOCK;
  self.wait_obj = lock;
  lock->writer_waiters.push_back(self.tid);

  schedule_next();
  return 0;
}

int ult_rwlock_unlock(ult_rwlock* lock) {
  ensure_initialized();
  if (!lock || !lock->initialized) {
    return -1;
  }
  check_preempt();

  SignalBlocker guard;
  if (lock->writer_active) {
    if (lock->writer != g_current_tid) {
      return -1;
    }
    lock->writer_active = false;
    lock->writer = -1;
  } else if (lock->readers > 0) {
    lock->readers--;
  } else {
    return -1;
  }

  if (lock->writer_active || lock->readers > 0) {
    return 0;
  }

  if (!lock->writer_waiters.empty()) {
    ult_tid_t next = lock->writer_waiters.front();
    lock->writer_waiters.pop_front();
    lock->waiting_writers--;
    lock->writer_active = true;
    lock->writer = next;

    Thread& waiter = g_threads[next];
    waiter.state = ThreadState::READY;
    waiter.wait_type = WaitType::NONE;
    waiter.wait_obj = nullptr;
    enqueue_ready(next);
    return 0;
  }

  while (!lock->reader_waiters.empty()) {
    ult_tid_t next = lock->reader_waiters.front();
    lock->reader_waiters.pop_front();
    lock->readers++;

    Thread& waiter = g_threads[next];
    waiter.state = ThreadState::READY;
    waiter.wait_type = WaitType::NONE;
    waiter.wait_obj = nullptr;
    enqueue_ready(next);
  }

  return 0;
}

int ult_rwlock_destroy(ult_rwlock* lock) {
  if (!lock || !lock->initialized) {
    return -1;
  }

  if (lock->writer_active || lock->readers > 0 || !lock->reader_waiters.empty() ||
      !lock->writer_waiters.empty()) {
    return -1;
  }

  lock->initialized = false;
  return 0;
}
