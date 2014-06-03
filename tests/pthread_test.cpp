/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include <errno.h>
#include <limits.h>
#include <malloc.h>
#include <pthread.h>
#include <unistd.h>

TEST(pthread, pthread_key_create) {
  pthread_key_t key;
  ASSERT_EQ(0, pthread_key_create(&key, NULL));
  ASSERT_EQ(0, pthread_key_delete(key));
  // Can't delete a key that's already been deleted.
  ASSERT_EQ(EINVAL, pthread_key_delete(key));
}

#if !defined(__GLIBC__) // glibc uses keys internally that its sysconf value doesn't account for.
TEST(pthread, pthread_key_create_lots) {
  // We can allocate _SC_THREAD_KEYS_MAX keys.
  std::vector<pthread_key_t> keys;
  for (int i = 0; i < sysconf(_SC_THREAD_KEYS_MAX); ++i) {
    pthread_key_t key;
    // If this fails, it's likely that GLOBAL_INIT_THREAD_LOCAL_BUFFER_COUNT is wrong.
    ASSERT_EQ(0, pthread_key_create(&key, NULL)) << i << " of " << sysconf(_SC_THREAD_KEYS_MAX);
    keys.push_back(key);
  }

  // ...and that really is the maximum.
  pthread_key_t key;
  ASSERT_EQ(EAGAIN, pthread_key_create(&key, NULL));

  // (Don't leak all those keys!)
  for (size_t i = 0; i < keys.size(); ++i) {
    ASSERT_EQ(0, pthread_key_delete(keys[i]));
  }
}
#endif

static void* IdFn(void* arg) {
  return arg;
}

static void* SleepFn(void* arg) {
  sleep(reinterpret_cast<unsigned int>(arg));
  return NULL;
}

static void* SpinFn(void* arg) {
  volatile bool* b = reinterpret_cast<volatile bool*>(arg);
  while (!*b) {
  }
  return NULL;
}

static void* JoinFn(void* arg) {
  return reinterpret_cast<void*>(pthread_join(reinterpret_cast<pthread_t>(arg), NULL));
}

static void AssertDetached(pthread_t t, bool is_detached) {
  pthread_attr_t attr;
  ASSERT_EQ(0, pthread_getattr_np(t, &attr));
  int detach_state;
  ASSERT_EQ(0, pthread_attr_getdetachstate(&attr, &detach_state));
  pthread_attr_destroy(&attr);
  ASSERT_EQ(is_detached, (detach_state == PTHREAD_CREATE_DETACHED));
}

static void MakeDeadThread(pthread_t& t) {
  ASSERT_EQ(0, pthread_create(&t, NULL, IdFn, NULL));
  void* result;
  ASSERT_EQ(0, pthread_join(t, &result));
}

TEST(pthread, pthread_create) {
  void* expected_result = reinterpret_cast<void*>(123);
  // Can we create a thread?
  pthread_t t;
  ASSERT_EQ(0, pthread_create(&t, NULL, IdFn, expected_result));
  // If we join, do we get the expected value back?
  void* result;
  ASSERT_EQ(0, pthread_join(t, &result));
  ASSERT_EQ(expected_result, result);
}

TEST(pthread, pthread_create_EAGAIN) {
  pthread_attr_t attributes;
  ASSERT_EQ(0, pthread_attr_init(&attributes));
  ASSERT_EQ(0, pthread_attr_setstacksize(&attributes, static_cast<size_t>(-1) & ~(getpagesize() - 1)));

  pthread_t t;
  ASSERT_EQ(EAGAIN, pthread_create(&t, &attributes, IdFn, NULL));
}

TEST(pthread, pthread_no_join_after_detach) {
  pthread_t t1;
  ASSERT_EQ(0, pthread_create(&t1, NULL, SleepFn, reinterpret_cast<void*>(5)));

  // After a pthread_detach...
  ASSERT_EQ(0, pthread_detach(t1));
  AssertDetached(t1, true);

  // ...pthread_join should fail.
  void* result;
  ASSERT_EQ(EINVAL, pthread_join(t1, &result));
}

TEST(pthread, pthread_no_op_detach_after_join) {
  bool done = false;

  pthread_t t1;
  ASSERT_EQ(0, pthread_create(&t1, NULL, SpinFn, &done));

  // If thread 2 is already waiting to join thread 1...
  pthread_t t2;
  ASSERT_EQ(0, pthread_create(&t2, NULL, JoinFn, reinterpret_cast<void*>(t1)));

  sleep(1); // (Give t2 a chance to call pthread_join.)

  // ...a call to pthread_detach on thread 1 will "succeed" (silently fail)...
  ASSERT_EQ(0, pthread_detach(t1));
  AssertDetached(t1, false);

  done = true;

  // ...but t2's join on t1 still goes ahead (which we can tell because our join on t2 finishes).
  void* join_result;
  ASSERT_EQ(0, pthread_join(t2, &join_result));
  ASSERT_EQ(0, reinterpret_cast<int>(join_result));
}

TEST(pthread, pthread_join_self) {
  void* result;
  ASSERT_EQ(EDEADLK, pthread_join(pthread_self(), &result));
}

#if __BIONIC__ // For some reason, gtest on bionic can cope with this but gtest on glibc can't.

static void TestBug37410() {
  pthread_t t1;
  ASSERT_EQ(0, pthread_create(&t1, NULL, JoinFn, reinterpret_cast<void*>(pthread_self())));
  pthread_exit(NULL);
}

// Even though this isn't really a death test, we have to say "DeathTest" here so gtest knows to
// run this test (which exits normally) in its own process.
TEST(pthread_DeathTest, pthread_bug_37410) {
  // http://code.google.com/p/android/issues/detail?id=37410
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  ASSERT_EXIT(TestBug37410(), ::testing::ExitedWithCode(0), "");
}
#endif

static void* SignalHandlerFn(void* arg) {
  sigset_t wait_set;
  sigfillset(&wait_set);
  return reinterpret_cast<void*>(sigwait(&wait_set, reinterpret_cast<int*>(arg)));
}

TEST(pthread, pthread_sigmask) {
  // Block SIGUSR1.
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  ASSERT_EQ(0, pthread_sigmask(SIG_BLOCK, &set, NULL));

  // Spawn a thread that calls sigwait and tells us what it received.
  pthread_t signal_thread;
  int received_signal = -1;
  ASSERT_EQ(0, pthread_create(&signal_thread, NULL, SignalHandlerFn, &received_signal));

  // Send that thread SIGUSR1.
  pthread_kill(signal_thread, SIGUSR1);

  // See what it got.
  void* join_result;
  ASSERT_EQ(0, pthread_join(signal_thread, &join_result));
  ASSERT_EQ(SIGUSR1, received_signal);
  ASSERT_EQ(0, reinterpret_cast<int>(join_result));
}

#if __BIONIC__
extern "C" int  __pthread_clone(void* (*fn)(void*), void* child_stack, int flags, void* arg);
TEST(pthread, __pthread_clone) {
  uintptr_t fake_child_stack[16];
  errno = 0;
  ASSERT_EQ(-1, __pthread_clone(NULL, &fake_child_stack[0], CLONE_THREAD, NULL));
  ASSERT_EQ(EINVAL, errno);
}
#endif

#if __BIONIC__ // Not all build servers have a new enough glibc? TODO: remove when they're on gprecise.
TEST(pthread, pthread_setname_np__too_long) {
  ASSERT_EQ(ERANGE, pthread_setname_np(pthread_self(), "this name is far too long for linux"));
}
#endif

#if __BIONIC__ // Not all build servers have a new enough glibc? TODO: remove when they're on gprecise.
TEST(pthread, pthread_setname_np__self) {
  ASSERT_EQ(0, pthread_setname_np(pthread_self(), "short 1"));
}
#endif

#if __BIONIC__ // Not all build servers have a new enough glibc? TODO: remove when they're on gprecise.
TEST(pthread, pthread_setname_np__other) {
  // Emulator kernels don't currently support setting the name of other threads.
  char* filename = NULL;
  asprintf(&filename, "/proc/self/task/%d/comm", gettid());
  struct stat sb;
  bool has_comm = (stat(filename, &sb) != -1);
  free(filename);

  if (has_comm) {
    pthread_t t1;
    ASSERT_EQ(0, pthread_create(&t1, NULL, SleepFn, reinterpret_cast<void*>(5)));
    ASSERT_EQ(0, pthread_setname_np(t1, "short 2"));
  } else {
    fprintf(stderr, "skipping test: this kernel doesn't have /proc/self/task/tid/comm files!\n");
  }
}
#endif

#if __BIONIC__ // Not all build servers have a new enough glibc? TODO: remove when they're on gprecise.
TEST(pthread, pthread_setname_np__no_such_thread) {
  pthread_t dead_thread;
  MakeDeadThread(dead_thread);

  // Call pthread_setname_np after thread has already exited.
  ASSERT_EQ(ESRCH, pthread_setname_np(dead_thread, "short 3"));
}
#endif

TEST(pthread, pthread_kill__0) {
  // Signal 0 just tests that the thread exists, so it's safe to call on ourselves.
  ASSERT_EQ(0, pthread_kill(pthread_self(), 0));
}

TEST(pthread, pthread_kill__invalid_signal) {
  ASSERT_EQ(EINVAL, pthread_kill(pthread_self(), -1));
}

static void pthread_kill__in_signal_handler_helper(int signal_number) {
  static int count = 0;
  ASSERT_EQ(SIGALRM, signal_number);
  if (++count == 1) {
    // Can we call pthread_kill from a signal handler?
    ASSERT_EQ(0, pthread_kill(pthread_self(), SIGALRM));
  }
}

TEST(pthread, pthread_kill__in_signal_handler) {
  struct sigaction action;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  action.sa_handler = pthread_kill__in_signal_handler_helper;
  sigaction(SIGALRM, &action, NULL);
  ASSERT_EQ(0, pthread_kill(pthread_self(), SIGALRM));
}

TEST(pthread, pthread_detach__no_such_thread) {
  pthread_t dead_thread;
  MakeDeadThread(dead_thread);

  ASSERT_EQ(ESRCH, pthread_detach(dead_thread));
}

TEST(pthread, pthread_detach__leak) {
  size_t initial_bytes = mallinfo().uordblks;

  pthread_attr_t attr;
  ASSERT_EQ(0, pthread_attr_init(&attr));
  ASSERT_EQ(0, pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE));

  std::vector<pthread_t> threads;
  for (size_t i = 0; i < 32; ++i) {
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, &attr, IdFn, NULL));
    threads.push_back(t);
  }

  sleep(1);

  for (size_t i = 0; i < 32; ++i) {
    ASSERT_EQ(0, pthread_detach(threads[i])) << i;
  }

  size_t final_bytes = mallinfo().uordblks;

  int leaked_bytes = (final_bytes - initial_bytes);

  // User code (like this test) doesn't know how large pthread_internal_t is.
  // We can be pretty sure it's more than 128 bytes.
  ASSERT_LT(leaked_bytes, 32 /*threads*/ * 128 /*bytes*/);
}

TEST(pthread, pthread_getcpuclockid__clock_gettime) {
  pthread_t t;
  ASSERT_EQ(0, pthread_create(&t, NULL, SleepFn, reinterpret_cast<void*>(5)));

  clockid_t c;
  ASSERT_EQ(0, pthread_getcpuclockid(t, &c));
  timespec ts;
  ASSERT_EQ(0, clock_gettime(c, &ts));
}

TEST(pthread, pthread_getcpuclockid__no_such_thread) {
  pthread_t dead_thread;
  MakeDeadThread(dead_thread);

  clockid_t c;
  ASSERT_EQ(ESRCH, pthread_getcpuclockid(dead_thread, &c));
}

TEST(pthread, pthread_getschedparam__no_such_thread) {
  pthread_t dead_thread;
  MakeDeadThread(dead_thread);

  int policy;
  sched_param param;
  ASSERT_EQ(ESRCH, pthread_getschedparam(dead_thread, &policy, &param));
}

TEST(pthread, pthread_setschedparam__no_such_thread) {
  pthread_t dead_thread;
  MakeDeadThread(dead_thread);

  int policy = 0;
  sched_param param;
  ASSERT_EQ(ESRCH, pthread_setschedparam(dead_thread, policy, &param));
}

TEST(pthread, pthread_join__no_such_thread) {
  pthread_t dead_thread;
  MakeDeadThread(dead_thread);

  void* result;
  ASSERT_EQ(ESRCH, pthread_join(dead_thread, &result));
}

TEST(pthread, pthread_kill__no_such_thread) {
  pthread_t dead_thread;
  MakeDeadThread(dead_thread);

  ASSERT_EQ(ESRCH, pthread_kill(dead_thread, 0));
}

TEST(pthread, pthread_join__multijoin) {
  bool done = false;

  pthread_t t1;
  ASSERT_EQ(0, pthread_create(&t1, NULL, SpinFn, &done));

  pthread_t t2;
  ASSERT_EQ(0, pthread_create(&t2, NULL, JoinFn, reinterpret_cast<void*>(t1)));

  sleep(1); // (Give t2 a chance to call pthread_join.)

  // Multiple joins to the same thread should fail.
  ASSERT_EQ(EINVAL, pthread_join(t1, NULL));

  done = true;

  // ...but t2's join on t1 still goes ahead (which we can tell because our join on t2 finishes).
  void* join_result;
  ASSERT_EQ(0, pthread_join(t2, &join_result));
  ASSERT_EQ(0, reinterpret_cast<int>(join_result));
}
