#include "ult.h"

#include <cstdio>

static ult_mutex g_mtx;
static int g_counter = 0;
static int g_in_critical = 0;

void* worker(void* arg) {
  int id = *reinterpret_cast<int*>(arg);
  for (int i = 0; i < 5; ++i) {
    if (ult_mutex_lock(&g_mtx) != 0) {
      std::printf("worker %d failed to lock\n", id);
      return nullptr;
    }

    if (g_in_critical != 0) {
      std::printf("ERROR: worker %d entered while %d holds lock\n", id,
                  g_in_critical);
    }
    g_in_critical = id;

    int before = g_counter;
    g_counter = before + 1;
    std::printf("worker %d enter: counter %d -> %d\n", id, before, g_counter);

    ult_yield();  // Others will try to lock, but must wait.

    std::printf("worker %d exit\n", id);
    g_in_critical = 0;

    ult_mutex_unlock(&g_mtx);
    ult_yield();
  }
  return nullptr;
}

int main() {
  ult_init(2000);
  ult_mutex_init(&g_mtx);

  ult_tid_t t1, t2, t3;
  int id1 = 1, id2 = 2, id3 = 3;

  ult_create(&t1, worker, &id1);
  ult_create(&t2, worker, &id2);
  ult_create(&t3, worker, &id3);

  ult_join(t1, nullptr);
  ult_join(t2, nullptr);
  ult_join(t3, nullptr);

  std::printf("final counter: %d (expected %d)\n", g_counter, 3 * 5);
  return 0;
}
