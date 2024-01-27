// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2013, Google, Inc. All rights reserved
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

/*
 * Functions for unit tests.  See lib/unittest/include/unittest.h for usage.
 */
#include <assert.h>
#include <debug.h>
#include <inttypes.h>
#include <lib/console.h>
#include <lib/fit/defer.h>
#include <lib/unittest/unittest.h>
#include <platform.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zircon/compiler.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <kernel/mp.h>
#include <kernel/mutex.h>
#include <kernel/thread.h>
#include <vm/vm_aspace.h>

namespace {

// Ensures unittests are not run concurrently.
DECLARE_SINGLETON_MUTEX(UnittestLock);
unsigned long g_repeat;

// External references to the testcase registration tables.
extern "C" unittest_testcase_registration_t __start_unittest_testcases[];
extern "C" unittest_testcase_registration_t __stop_unittest_testcases[];

void usage(const char* progname) {
  printf(
      "Usage:\n"
      "%s <case>\n"
      "  where case is a specific testcase name, or...\n"
      "  all : run all tests\n"
      "  ?   : list tests\n"
      "  [-r num]  : repeat a test case num times\n",
      progname);
}

void list_cases(void) {
  size_t count = 0;
  size_t max_namelen = 0;

  const unittest_testcase_registration_t* testcase;
  for (testcase = __start_unittest_testcases; testcase != __stop_unittest_testcases; ++testcase) {
    if (testcase->name) {
      size_t namelen = strlen(testcase->name);
      if (max_namelen < namelen)
        max_namelen = namelen;
      count++;
    }
  }

  printf("There %s %zu test case%s available...\n", count == 1 ? "is" : "are", count,
         count == 1 ? "" : "s");

  for (testcase = __start_unittest_testcases; testcase != __stop_unittest_testcases; ++testcase) {
    if (testcase->name)
      printf("  %-*s : %s\n", static_cast<int>(max_namelen), testcase->name,
             testcase->desc ? testcase->desc : "<no description>");
  }
}

bool run_unittest(const unittest_testcase_registration_t* testcase) {
  size_t max_namelen = 0;
  size_t passed = 0;

  DEBUG_ASSERT(testcase);
  DEBUG_ASSERT(testcase->name);
  DEBUG_ASSERT(!!testcase->tests == !!testcase->test_cnt);

  for (size_t i = 0; i < testcase->test_cnt; ++i) {
    const unittest_registration_t* test = &testcase->tests[i];
    if (test->name) {
      size_t namelen = strlen(test->name);
      if (max_namelen < namelen)
        max_namelen = namelen;
    }
  }

  unittest_printf("%s : Running %zu test%s...\n", testcase->name, testcase->test_cnt,
                  testcase->test_cnt == 1 ? "" : "s");

  zx_time_t testcase_start = current_time();

  for (unsigned long j = 0; j < g_repeat; j++) {
    bool good = true;
    for (size_t i = 0; i < testcase->test_cnt; ++i) {
      const unittest_registration_t* test = &testcase->tests[i];

      unittest_printf("  %-*s : ", static_cast<int>(max_namelen), test->name ? test->name : "");

      zx_time_t test_start = current_time();
      good &= test->fn ? test->fn() : false;
      zx_duration_t test_runtime = current_time() - test_start;

      unittest_printf("%s (%" PRIi64 " nSec) ", good ? "PASSED" : "FAILED", test_runtime);
      if (g_repeat > 1) {
        unittest_printf(" [%lu / %lu]", j + 1, g_repeat);
      }
      unittest_printf("\n");
      if (good) {
        if (j == g_repeat - 1) {
          passed++;
        }
      } else {
        printf("  %-*s : ", static_cast<int>(max_namelen), test->name ? test->name : "");
        break;
      }
    }
  }

  zx_duration_t testcase_runtime = current_time() - testcase_start;

  unittest_printf("%s : %sll tests passed (%zu/%zu) in %" PRIi64 " nSec\n", testcase->name,
                  passed != testcase->test_cnt ? "Not a" : "A", passed, testcase->test_cnt,
                  testcase_runtime);

  return passed == testcase->test_cnt;
}

// Runs the testcase specified by |arg| and returns 1 if test passes.
//
// |arg| is a const unittest_testcase_registration_t*.
int run_unittest_thread_entry(void* arg) {
  auto* testcase = static_cast<const unittest_testcase_registration_t*>(arg);
  return run_unittest(testcase);
}

// Runs |testcase| in another thread and waits for it to complete.
//
// Returns true if the test passed.
bool run_testcase_in_thread(const unittest_testcase_registration_t* testcase) {
  fbl::RefPtr<VmAspace> aspace = VmAspace::Create(VmAspace::Type::User, "unittest");
  if (!aspace) {
    unittest_printf("failed to create unittest user aspace\n");
    return false;
  }
  auto destroy_aspace = fit::defer([&]() {
    zx_status_t status = aspace->Destroy();
    DEBUG_ASSERT(status == ZX_OK);
  });
  Thread* t =
      Thread::Create("unittest", run_unittest_thread_entry,
                     const_cast<void*>(static_cast<const void*>(testcase)), DEFAULT_PRIORITY);
  if (!t) {
    unittest_printf("failed to create unittest thread\n");
    return false;
  }
  aspace->AttachToThread(t);

  const cpu_mask_t online_mask_before = mp_get_online_mask();
  const cpu_mask_t active_mask_before = mp_get_active_mask();

  t->Resume();
  int success = 0;
  zx_status_t status = t->Join(&success, ZX_TIME_INFINITE);
  if (status != ZX_OK) {
    unittest_printf("failed to join unittest thread: %d\n", status);
    return false;
  }

  // Make sure that |testcase| didn't change the online or active state of any CPUs.
  const cpu_mask_t online_mask_after = mp_get_online_mask();
  const cpu_mask_t active_mask_after = mp_get_active_mask();
  ASSERT_MSG(online_mask_after == online_mask_before, "name=%s after=0x%08x before=0x%08x\n",
             testcase->name, online_mask_after, online_mask_before);
  ASSERT_MSG(active_mask_after == active_mask_before, "name=%s after=0x%08x before=0x%08x\n",
             testcase->name, active_mask_after, active_mask_before);

  return success;
}

int run_unittests_locked(int argc, const cmd_args* argv, uint32_t flags)
    TA_REQ(UnittestLock::Get()) {
  DEBUG_ASSERT(UnittestLock::Get()->lock().IsHeld());
  if (argc < 2) {
    usage(argv[0].str);
    return 0;
  }

  const char* casename = argv[1].str;

  if (!strcmp(casename, "?")) {
    list_cases();
    return 0;
  }
  g_repeat = 1;
  if (!strcmp(casename, "-r")) {
    if (argc < 4) {
      usage(argv[0].str);
      return 0;
    }
    g_repeat = argv[2].u;
    casename = argv[3].str;
  }

  bool run_all = !strcmp(casename, "all");
  const unittest_testcase_registration_t* testcase;
  size_t chosen = 0;
  size_t passed = 0;

  const size_t num_tests = run_all ? __stop_unittest_testcases - __start_unittest_testcases : 1;
  // Array of names with a NULL sentinel at the end.
  const char** failed_names = static_cast<const char**>(calloc(num_tests + 1, sizeof(char*)));
  const char** fn = failed_names;

  for (testcase = __start_unittest_testcases; testcase != __stop_unittest_testcases; ++testcase) {
    if (testcase->name) {
      if (run_all || !strcmp(casename, testcase->name)) {
        chosen++;

        bool status = run_testcase_in_thread(testcase);
        printf("\n");
        if (status) {
          passed++;
        } else {
          *fn++ = testcase->name;
        }

        if (!run_all)
          break;
      }
    }
  }

  int ret = 0;
  if (!run_all && !chosen) {
    ret = -1;
    unittest_printf("Test case \"%s\" not found!\n", casename);
    list_cases();
  } else {
    unittest_printf("SUMMARY: Ran %zu test case%s: %zu failed\n", chosen, chosen == 1 ? "" : "s",
                    chosen - passed);
    if (passed < chosen) {
      ret = -1;
      unittest_printf("\nThe following test cases failed:\n");
      for (fn = failed_names; *fn != NULL; fn++) {
        unittest_printf("%s\n", *fn);
      }
    }
  }

  free(failed_names);
  return ret;
}

int run_unittests(int argc, const cmd_args* argv, uint32_t flags) {
  Guard<Mutex> guard{UnittestLock::Get()};
  return run_unittests_locked(argc, argv, flags);
}

}  // namespace

STATIC_COMMAND_START
STATIC_COMMAND("ut", "Run unittests", run_unittests)
STATIC_COMMAND_END(unittests)
