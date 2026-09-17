// Minimal Haiku kernel-kit shim so the runtime builds headless on macOS/Linux for testing.
#pragma once
#include <cstdint>
#include <ctime>
#include <thread>
#include <atomic>
#include <chrono>
#include <sys/time.h>
#include <unistd.h>
typedef int64_t bigtime_t;
typedef int32_t int32;
typedef uint32_t uint32;
typedef int32_t thread_id;
typedef int32 (*thread_func)(void*);
typedef int32_t status_t;
enum { B_OK = 0 };
enum { B_NORMAL_PRIORITY = 10, B_DISPLAY_PRIORITY = 15, B_URGENT_DISPLAY_PRIORITY = 20 };
static inline bigtime_t system_time() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static inline bigtime_t real_time_clock_usecs() { struct timeval tv; gettimeofday(&tv, NULL); return bigtime_t(tv.tv_sec) * 1000000 + tv.tv_usec; }
static inline void snooze(bigtime_t us) { if (us > 0) usleep(useconds_t(us)); }
static inline int32 atomic_test_and_set(int32* v, int32 n, int32 test) { return __sync_val_compare_and_swap(v, test, n); }
static inline int32 atomic_set(int32* v, int32 n) { return __sync_lock_test_and_set(v, n); }
static inline int32 atomic_add(int32* v, int32 n) { return __sync_fetch_and_add(v, n); }
static inline int32 atomic_or(int32* v, int32 n) { return __sync_fetch_and_or(v, n); }
static inline int32 atomic_and(int32* v, int32 n) { return __sync_fetch_and_and(v, n); }
thread_id spawn_thread(thread_func f, const char* name, int32 prio, void* arg);
static inline status_t resume_thread(thread_id) { return B_OK; }
