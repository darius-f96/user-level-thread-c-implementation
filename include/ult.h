#ifndef ULT_H
#define ULT_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

using ult_tid_t = int;

// Thread API
int ult_init(unsigned int quantum_us);
int ult_create(ult_tid_t* tid, void* (*start_routine)(void*), void* arg);
ult_tid_t ult_self();
int ult_join(ult_tid_t tid, void** retval);
void ult_exit(void* retval);
int ult_yield();

// Mutex API
struct ult_mutex {
  bool initialized = false;
  bool locked = false;
  ult_tid_t owner = -1;
  std::deque<ult_tid_t> waiters;
};

int ult_mutex_init(ult_mutex* mtx);
int ult_mutex_lock(ult_mutex* mtx);
int ult_mutex_unlock(ult_mutex* mtx);

// Read-write lock API (writer preference)
struct ult_rwlock {
  bool initialized = false;
  bool writer_active = false;
  ult_tid_t writer = -1;
  int readers = 0;
  int waiting_writers = 0;
  std::deque<ult_tid_t> reader_waiters;
  std::deque<ult_tid_t> writer_waiters;
};

int ult_rwlock_init(ult_rwlock* lock);
int ult_rwlock_rdlock(ult_rwlock* lock);
int ult_rwlock_wrlock(ult_rwlock* lock);
int ult_rwlock_unlock(ult_rwlock* lock);
int ult_rwlock_destroy(ult_rwlock* lock);

#endif
