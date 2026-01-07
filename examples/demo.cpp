#include "ult.h"

#include <cstdio>
#include <ctime>
#include <string>
#include <unistd.h>

std::string format_timestamp() {
  timespec ts{};
  clock_gettime(CLOCK_REALTIME, &ts);
  tm tm_now{};
  localtime_r(&ts.tv_sec, &tm_now);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%06ld",
                tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec,
                ts.tv_nsec / 1000);
  return std::string(buf);
}

FILE* open_worker_log(int id) {
  std::string name = "worker_" + std::to_string(id) + ".log";
  return std::fopen(name.c_str(), "w");
}

void* worker(void* arg) {
  int id = *reinterpret_cast<int*>(arg);
  FILE* log = open_worker_log(id);
  if (!log) {
    return nullptr;
  }
  for (int i = 0; i < 5; ++i) {
    std::string ts = format_timestamp();
    std::fprintf(log, "[%s] worker %d iteration %d\n", ts.c_str(), id, i);
    std::fflush(log);
    ult_yield();
    //usleep(1000000);
  }
  std::fclose(log);
  return reinterpret_cast<void*>(static_cast<intptr_t>(id * 10));
}

void* spinner(void*) {
  volatile unsigned long x = 0;
  while (x < 500000000UL) { x++; }
  return nullptr;
}

void* printer(void*) {
  for (int i = 0; i < 10; i++) {
    std::string ts = format_timestamp();
    printf("tick %d time: %s\n", i, ts.c_str());
  }
  return nullptr;
}

int main() {
  ult_init(2000);

  ult_tid_t t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12;
  int a = 1;
  int b = 2;
  int c = 3;
  int d = 4;
  int e = 5;
  int f = 6;
  int g = 7;
  int h = 8;
  int i = 9;
  int j = 10;

  ult_create(&t1, worker, &a);
  ult_create(&t2, worker, &b);
  ult_create(&t3, worker, &c);
  ult_create(&t4, worker, &d);
  ult_create(&t5, worker, &e);
  ult_create(&t6, worker, &f);
  ult_create(&t7, worker, &g);
  ult_create(&t8, worker, &h);
  ult_create(&t9, worker, &i);
  ult_create(&t10, worker, &j);
  ult_create(&t11, spinner, nullptr);
  ult_create(&t12, printer, nullptr);

  void* r1 = nullptr;
  void* r2 = nullptr;
  void* r3 = nullptr;
  void* r4 = nullptr;
  void* r5 = nullptr;
  void* r6 = nullptr;
  void* r7 = nullptr;
  void* r8 = nullptr;
  void* r9 = nullptr;
  void* r10 = nullptr;

  ult_join(t1, &r1);
  ult_join(t2, &r2);
  ult_join(t3, &r3);
  ult_join(t4, &r4);
  ult_join(t5, &r5);
  ult_join(t6, &r6);
  ult_join(t7, &r7);
  ult_join(t8, &r8);
  ult_join(t9, &r9);
  ult_join(t10, &r10);
  ult_join(t11, nullptr);
  ult_join(t12, nullptr);

  printf("joined results: %ld %ld %ld %ld %ld %ld %ld %ld %ld %ld\n", (long)r1, (long)r2, (long)r3, (long)r4, (long)r5, (long)r6, (long)r7, (long)r8, (long)r9, (long)r10);
  return 0;
}
