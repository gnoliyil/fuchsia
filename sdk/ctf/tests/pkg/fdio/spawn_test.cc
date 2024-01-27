// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// General fdio_spawn tests

#include <errno.h>
#include <fuchsia/io/cpp/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/io.h>
#include <lib/fdio/namespace.h>
#include <lib/fdio/spawn.h>
#include <lib/zx/channel.h>
#include <lib/zx/clock.h>
#include <lib/zx/job.h>
#include <lib/zx/process.h>
#include <lib/zx/socket.h>
#include <lib/zx/vmo.h>
#include <stdlib.h>
#include <unistd.h>
#include <zircon/limits.h>
#include <zircon/processargs.h>
#include <zircon/syscalls/policy.h>
#include <zircon/utc.h>

#include <zxtest/zxtest.h>

#include "fake_launcher_util.h"

namespace fio = ::fuchsia::io;

namespace {

#define FDIO_SPAWN_CLONE_ALL_EXCEPT_NS (FDIO_SPAWN_CLONE_ALL & ~FDIO_SPAWN_CLONE_NAMESPACE)

static constexpr char kSpawnChild[] = "/pkg/bin/spawn_child_util";
static constexpr char kSpawnLauncher[] = "/pkg/bin/fake_launcher_util";

static bool has_fd(int fd) {
  zx::handle handle;
  zx_status_t status = fdio_fd_clone(fd, handle.reset_and_get_address());
  if (status == ZX_OK) {
    return true;
  }
  return false;
}

static void join(const zx::process& process, int64_t* return_code) {
  zx_status_t status = process.wait_one(ZX_TASK_TERMINATED, zx::time::infinite(), nullptr);
  ASSERT_OK(status);

  zx_info_process_t proc_info{};
  status = process.get_info(ZX_INFO_PROCESS, &proc_info, sizeof(proc_info), nullptr, nullptr);
  ASSERT_OK(status);

  *return_code = proc_info.return_code;
}

TEST(SpawnTest, SpawnControl) {
  zx::process process;
  int64_t return_code;
  const char* bin_path = kSpawnChild;

  {
    const char* argv[] = {bin_path, nullptr};
    ASSERT_OK(fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv,
                         process.reset_and_get_address()));
    join(process, &return_code);
    EXPECT_EQ(return_code, 43);
  }

  {
    const char* argv[] = {bin_path, "--argc", nullptr};
    ASSERT_OK(fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv,
                         process.reset_and_get_address()));
    join(process, &return_code);
    EXPECT_EQ(return_code, 2);
  }

  {
    const char* argv[] = {bin_path, "--argc", "three", "four", "five", nullptr};
    ASSERT_OK(fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv,
                         process.reset_and_get_address()));
    join(process, &return_code);
    EXPECT_EQ(return_code, 5);
  }
}

TEST(SpawnTest, BinPaths) {
  zx::process process;
  int64_t return_code;
  const char* argv[] = {"spawn_child_util", nullptr};

  // Absolute path
  constexpr char kAbsoluteBinPath[] = "/pkg/bin/spawn_child_util";
  {
    ASSERT_OK(fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, kAbsoluteBinPath, argv,
                         process.reset_and_get_address()));
    join(process, &return_code);
    EXPECT_EQ(return_code, 43);
  }

  // Relative path with CWD set to "/"
  constexpr char kRootRelativeBinPath[] = "pkg/bin/spawn_child_util";
  {
    ASSERT_OK(fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, kRootRelativeBinPath, argv,
                         process.reset_and_get_address()));
    join(process, &return_code);
    EXPECT_EQ(return_code, 43);
  }

  // Relative path with CWD set to "/pkg"
  constexpr char kPkgRelativeBinPath[] = "bin/spawn_child_util";
  ASSERT_EQ(0, chdir("/pkg"), "errno %d: %s", errno, strerror(errno));
  {
    ASSERT_OK(fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, kPkgRelativeBinPath, argv,
                         process.reset_and_get_address()));
    join(process, &return_code);
    EXPECT_EQ(return_code, 43);
  }
  ASSERT_EQ(0, chdir("/"), "errno %d: %s", errno, strerror(errno));

  // Non-canonical form of a root relative path.
  constexpr char kNonCanonicalRelativeBinPath[] = "./pkg/foo/../bin/spawn_child_util";
  {
    ASSERT_OK(fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, kNonCanonicalRelativeBinPath,
                         argv, process.reset_and_get_address()));
    join(process, &return_code);
    EXPECT_EQ(return_code, 43);
  }
}

TEST(SpawnTest, SpawnLauncher) {
  zx_status_t status;
  zx::process process;
  int64_t return_code;
  const char* launcher_bin_path = kSpawnLauncher;
  const char* argv[] = {launcher_bin_path, nullptr};

  // Check that setting |ZX_POL_NEW_PROCESS| to |ZX_POL_ACTION_DENY| prevents
  // the launcher from launching the child.
  {
    zx::job job;
    ASSERT_OK(zx::job::create(*zx::job::default_job(), 0, &job));
    zx_policy_basic_v2_t policy = {.condition = ZX_POL_NEW_PROCESS,
                                   .action = ZX_POL_ACTION_DENY,
                                   .flags = ZX_POL_OVERRIDE_DENY};
    ASSERT_OK(job.set_policy(ZX_JOB_POL_RELATIVE, ZX_JOB_POL_BASIC_V2, &policy, 1));

    status = fdio_spawn(job.get(), FDIO_SPAWN_CLONE_ALL, launcher_bin_path, argv,
                        process.reset_and_get_address());
    ASSERT_OK(status);
    join(process, &return_code);
    EXPECT_EQ(return_code, LAUNCHER_FAILURE);
    ASSERT_OK(job.kill());
  }
}

TEST(SpawnTest, SpawnNested) {
  zx_status_t status;
  zx::process process;
  int64_t return_code;
  const char* bin_path = kSpawnChild;

  {
    const char* argv[] = {bin_path, "--spawn", bin_path, nullptr};
    int flags = FDIO_SPAWN_DEFAULT_LDSVC | FDIO_SPAWN_CLONE_NAMESPACE | FDIO_SPAWN_CLONE_JOB;
    status = fdio_spawn(ZX_HANDLE_INVALID, flags, bin_path, argv, process.reset_and_get_address());
    ASSERT_OK(status);
    join(process, &return_code);
    EXPECT_EQ(return_code, 43);
  }

  {
    const char* argv[] = {bin_path, "--spawn", bin_path, nullptr};
    status = fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv,
                        process.reset_and_get_address());
    ASSERT_OK(status);
    join(process, &return_code);
    EXPECT_EQ(return_code, 43);
  }

  {
    setenv("DUMMY_ENV", "1", 1);

    const char* argv[] = {bin_path, "--spawn", bin_path, "--flags", "all", nullptr};
    status = fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv,
                        process.reset_and_get_address());
    ASSERT_OK(status);
    join(process, &return_code);
    EXPECT_EQ(return_code, 56);

    unsetenv("DUMMY_ENV");
  }
}

TEST(SpawnTest, SpawnInvalidArgs) {
  zx_status_t status;
  zx::process process;
  const char* bin_path = kSpawnChild;
  const char* argv[] = {bin_path, nullptr};

  status = fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, "/bogus/not/a/file", argv,
                      process.reset_and_get_address());
  ASSERT_EQ(ZX_ERR_NOT_FOUND, status);

  status = fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, NULL,
                      process.reset_and_get_address());
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, status);

  status = fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv + 1,
                      process.reset_and_get_address());
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, status);
}

TEST(SpawnTest, SpawnFlags) {
  zx_status_t status;
  zx::process process;
  int64_t return_code;
  const char* bin_path = kSpawnChild;

  {
    // We can't actually launch a process without FDIO_SPAWN_DEFAULT_LDSVC
    // because we can't load the PT_INTERP.
    const char* argv[] = {bin_path, "--flags", "none", nullptr};
    status = fdio_spawn(ZX_HANDLE_INVALID, 0, bin_path, argv, process.reset_and_get_address());
    ASSERT_EQ(ZX_ERR_INVALID_ARGS, status);
    EXPECT_FALSE(process.is_valid());
  }

  {
    const char* argv[] = {bin_path, "--flags", "none", nullptr};
    status = fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_DEFAULT_LDSVC, bin_path, argv,
                        process.reset_and_get_address());
    ASSERT_OK(status);
    join(process, &return_code);
    EXPECT_EQ(return_code, 51);
  }

  {
    const char* argv[] = {bin_path, "--flags", "job", nullptr};
    status = fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_JOB | FDIO_SPAWN_DEFAULT_LDSVC,
                        bin_path, argv, process.reset_and_get_address());
    ASSERT_OK(status);
    join(process, &return_code);
    EXPECT_EQ(return_code, 52);
  }

  {
    const char* argv[] = {bin_path, "--flags", "namespace", nullptr};
    status = fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_DEFAULT_LDSVC | FDIO_SPAWN_CLONE_NAMESPACE,
                        bin_path, argv, process.reset_and_get_address());
    ASSERT_OK(status);
    join(process, &return_code);
    EXPECT_EQ(return_code, 53);
  }

  {
    const char* argv[] = {bin_path, "--flags", "stdio", nullptr};
    status = fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_DEFAULT_LDSVC | FDIO_SPAWN_CLONE_STDIO,
                        bin_path, argv, process.reset_and_get_address());
    ASSERT_OK(status);
    join(process, &return_code);
    EXPECT_EQ(return_code, 54);
  }

  {
    // Redundant CLONE_STDIO vs individual fd actions should work.
    ASSERT_EQ(0, dup2(1, 0));
    const char* argv[] = {bin_path, "--flags", "stdio", nullptr};
    char err_msg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];
    const fdio_spawn_action_t actions[] = {
        {.action = FDIO_SPAWN_ACTION_CLONE_FD, .fd = {.local_fd = 0, .target_fd = 0}},
        {.action = FDIO_SPAWN_ACTION_CLONE_FD, .fd = {.local_fd = 1, .target_fd = 1}},
        {.action = FDIO_SPAWN_ACTION_CLONE_FD, .fd = {.local_fd = 2, .target_fd = 2}},
    };
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_DEFAULT_LDSVC | FDIO_SPAWN_CLONE_STDIO,
                            bin_path, argv, nullptr, std::size(actions), actions,
                            process.reset_and_get_address(), err_msg);
    ASSERT_OK(status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(return_code, 54);
  }

  {
    setenv("DUMMY_ENV", "1", 1);

    const char* argv[] = {bin_path, "--flags", "environ", nullptr};
    status = fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_DEFAULT_LDSVC | FDIO_SPAWN_CLONE_ENVIRON,
                        bin_path, argv, process.reset_and_get_address());
    ASSERT_OK(status);
    join(process, &return_code);
    EXPECT_EQ(return_code, 55);

    unsetenv("DUMMY_ENV");
  }

  {
    setenv("DUMMY_ENV", "1", 1);

    const char* argv[] = {bin_path, "--flags", "all", nullptr};
    status = fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv,
                        process.reset_and_get_address());
    ASSERT_OK(status);
    join(process, &return_code);
    EXPECT_EQ(return_code, 56);

    unsetenv("DUMMY_ENV");
  }
}

TEST(SpawnTest, SpawnEnviron) {
  zx_status_t status;
  zx::process process;
  int64_t return_code;
  const char* bin_path = kSpawnChild;
  char err_msg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];

  setenv("SPAWN_TEST_PARENT", "1", 1);

  {
    const char* argv[] = {bin_path, "--env", "empty", nullptr};
    const char* env[] = {nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_DEFAULT_LDSVC, bin_path, argv, env, 0,
                            nullptr, process.reset_and_get_address(), err_msg);
    ASSERT_OK(status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(return_code, 61);
  }

  {
    const char* argv[] = {bin_path, "--env", "one", nullptr};
    const char* env[] = {"SPAWN_TEST_CHILD=1", nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_DEFAULT_LDSVC, bin_path, argv, env, 0,
                            nullptr, process.reset_and_get_address(), err_msg);
    ASSERT_OK(status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(return_code, 62);
  }

  {
    const char* argv[] = {bin_path, "--env", "one", nullptr};
    const char* env[] = {"SPAWN_TEST_CHILD=1", nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, env, 0,
                            nullptr, process.reset_and_get_address(), err_msg);
    ASSERT_OK(status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(return_code, 62);
  }

  {
    const char* argv[] = {bin_path, "--env", "two", nullptr};
    const char* env[] = {"SPAWN_TEST_CHILD=1", "SPAWN_TEST_CHILD2=1", nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, env, 0,
                            nullptr, process.reset_and_get_address(), err_msg);
    ASSERT_OK(status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(return_code, 63);
  }

  {
    const char* argv[] = {bin_path, "--env", "clone", nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, nullptr, 0,
                            nullptr, process.reset_and_get_address(), err_msg);
    ASSERT_OK(status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(return_code, 64);
  }

  unsetenv("SPAWN_TEST_PARENT");
}

TEST(SpawnTest, SpawnActionsFd) {
  zx_status_t status;
  zx::process process;
  int64_t return_code;
  const char* bin_path = kSpawnChild;
  char err_msg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];

  {
    const char* argv[] = {nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, nullptr, 0,
                            nullptr, process.reset_and_get_address(), err_msg);
    ASSERT_EQ(ZX_ERR_INVALID_ARGS, status, "%s", err_msg);
  }

  {
    fdio_spawn_action_t action;
    action.action = FDIO_SPAWN_ACTION_SET_NAME;
    action.name.data = "spawn-child-name";

    const char* argv[] = {nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, nullptr, 1,
                            &action, process.reset_and_get_address(), err_msg);
    ASSERT_OK(status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(return_code, 42);

    char name[ZX_MAX_NAME_LEN];
    ASSERT_OK(process.get_property(ZX_PROP_NAME, name, sizeof(name)));
    EXPECT_TRUE(!strcmp("spawn-child-name", name));
  }

  {
    int fd;
    zx::socket socket;
    status = fdio_pipe_half(&fd, socket.reset_and_get_address());
    ASSERT_GE(status, ZX_OK);

    fdio_spawn_action_t action;
    action.action = FDIO_SPAWN_ACTION_CLONE_FD;
    action.fd.local_fd = fd;
    action.fd.target_fd = 21;

    const char* argv[] = {bin_path, "--action", "clone-fd", nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, nullptr, 1,
                            &action, process.reset_and_get_address(), err_msg);
    ASSERT_OK(status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(return_code, 71);
    EXPECT_TRUE(has_fd(fd));
    close(fd);
  }

  {
    zx::socket socket;
    int fd;
    status = fdio_pipe_half(&fd, socket.reset_and_get_address());
    ASSERT_GE(status, ZX_OK);

    fdio_spawn_action_t action;
    action.action = FDIO_SPAWN_ACTION_TRANSFER_FD;
    action.fd.local_fd = fd;
    action.fd.target_fd = 21;

    const char* argv[] = {bin_path, "--action", "transfer-fd", nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, nullptr, 1,
                            &action, process.reset_and_get_address(), err_msg);
    ASSERT_OK(status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(return_code, 72);
    EXPECT_FALSE(has_fd(fd));
  }

  {
    zx::socket socket;
    int fd;
    status = fdio_pipe_half(&fd, socket.reset_and_get_address());
    ASSERT_GE(status, ZX_OK);

    fdio_spawn_action_t actions[2];
    actions[0].action = FDIO_SPAWN_ACTION_CLONE_FD;
    actions[0].fd.local_fd = fd;
    actions[0].fd.target_fd = 21;
    actions[1].action = FDIO_SPAWN_ACTION_TRANSFER_FD;
    actions[1].fd.local_fd = fd;
    actions[1].fd.target_fd = 22;

    const char* argv[] = {bin_path, "--action", "clone-and-transfer-fd", nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, nullptr, 2,
                            actions, process.reset_and_get_address(), err_msg);
    ASSERT_OK(status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(return_code, 73);
    EXPECT_FALSE(has_fd(fd));
  }
}

TEST(SpawnTest, SpawnActionsAddNamespaceEntry) {
  zx_status_t status;
  zx::process process;
  int64_t return_code;
  const char* bin_path = kSpawnChild;
  char err_msg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];

  {
    zx::channel h1, h2;
    ASSERT_OK(zx::channel::create(0, &h1, &h2));

    fdio_spawn_action_t action;
    action.action = FDIO_SPAWN_ACTION_ADD_NS_ENTRY;
    action.ns.prefix = "/foo/bar/baz";
    action.ns.handle = h1.release();

    const char* argv[] = {bin_path, "--action", "ns-entry", nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, nullptr, 1,
                            &action, process.reset_and_get_address(), err_msg);
    ASSERT_OK(status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(return_code, 74);
  }
}

TEST(SpawnTest, SpawnActionAddHandle) {
  zx_status_t status;
  zx::process process;
  int64_t return_code;
  const char* bin_path = kSpawnChild;
  char err_msg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];

  {
    zx::channel h1, h2;
    ASSERT_OK(zx::channel::create(0, &h1, &h2));

    fdio_spawn_action_t action;
    action.action = FDIO_SPAWN_ACTION_ADD_HANDLE;
    action.h.id = PA_USER0;
    action.h.handle = h1.release();

    const char* argv[] = {bin_path, "--action", "add-handle", nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, nullptr, 1,
                            &action, process.reset_and_get_address(), err_msg);
    ASSERT_OK(status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(return_code, 75);
  }
}

TEST(SpawnTest, SpawnActionsSetName) {
  zx_status_t status;
  zx::process process;
  int64_t return_code;
  const char* bin_path = kSpawnChild;
  char err_msg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];

  {
    fdio_spawn_action_t actions[2];
    actions[0].action = FDIO_SPAWN_ACTION_SET_NAME;
    actions[0].name.data = "proc-name-0";
    actions[1].action = FDIO_SPAWN_ACTION_SET_NAME;
    actions[1].name.data = "proc-name-1";

    const char* argv[] = {bin_path, nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, nullptr, 2,
                            actions, process.reset_and_get_address(), err_msg);
    ASSERT_OK(status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(return_code, 43);
    char name[ZX_MAX_NAME_LEN];
    ASSERT_OK(process.get_property(ZX_PROP_NAME, name, sizeof(name)));
    EXPECT_EQ(0, strcmp(name, "proc-name-1"));
  }
}

TEST(SpawnTest, SpawnActionsCloneDir) {
  zx_status_t status;
  zx::process process;
  int64_t return_code;
  const char* bin_path = kSpawnChild;
  char err_msg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];

  {
    fdio_spawn_action_t action;
    action.action = FDIO_SPAWN_ACTION_CLONE_DIR;
    action.dir.prefix = "/";

    const char* argv[] = {bin_path, "--flags", "namespace", nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_DEFAULT_LDSVC, bin_path, argv, nullptr, 1,
                            &action, process.reset_and_get_address(), err_msg);
    ASSERT_OK(status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(return_code, 53);
  }

  {
    fdio_spawn_action_t action;
    action.action = FDIO_SPAWN_ACTION_CLONE_DIR;
    action.dir.prefix = "/foo/bar/baz";

    zx::channel h1, h2;
    ASSERT_OK(zx::channel::create(0, &h1, &h2));
    fdio_ns_t* ns = nullptr;
    ASSERT_OK(fdio_ns_get_installed(&ns));

    EXPECT_FALSE(fdio_ns_is_bound(ns, nullptr));
    EXPECT_FALSE(fdio_ns_is_bound(ns, ""));
    EXPECT_FALSE(fdio_ns_is_bound(ns, "/"));
    EXPECT_FALSE(fdio_ns_is_bound(ns, "/foo/bar/baz"));

    ASSERT_OK(fdio_ns_bind(ns, "/foo/bar/baz", h1.release()));

    EXPECT_FALSE(fdio_ns_is_bound(ns, "/foo/bar"));
    EXPECT_TRUE(fdio_ns_is_bound(ns, "/foo/bar/baz"));
    EXPECT_FALSE(fdio_ns_is_bound(ns, "/foo/bar/baz/qux"));

    const char* argv[] = {bin_path, "--action", "ns-entry", nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL_EXCEPT_NS, bin_path, argv,
                            nullptr, 1, &action, process.reset_and_get_address(), err_msg);
    EXPECT_EQ(ZX_OK, status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(return_code, 74);

    // Unbind the test namespace.
    EXPECT_EQ(ZX_OK, fdio_ns_unbind(ns, "/foo/bar/baz"));
  }

  {
    // Test using a directory prefix. In this case, sharing /foo/bar should provide access to
    // the /foo/bar/baz namespace.
    fdio_spawn_action_t action;
    action.action = FDIO_SPAWN_ACTION_CLONE_DIR;
    action.dir.prefix = "/foo/bar";

    fdio_ns_t* ns = nullptr;
    ASSERT_OK(fdio_ns_get_installed(&ns));

    zx::channel h1, h2;
    ASSERT_OK(zx::channel::create(0, &h1, &h2));
    ASSERT_OK(fdio_ns_bind(ns, "/foo/bar/baz", h1.release()));

    const char* argv[] = {bin_path, "--stat", "/foo/bar", nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL_EXCEPT_NS, bin_path, argv,
                            nullptr, 1, &action, process.reset_and_get_address(), err_msg);
    EXPECT_EQ(ZX_OK, status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(return_code, 76);

    // Unbind the test namespace.
    EXPECT_EQ(ZX_OK, fdio_ns_unbind(ns, "/foo/bar/baz"));
  }

  {
    // Verify we don't match paths in the middle of directory names. In this case, verify
    // that /foo/bar/baz does not match as a prefix to the directory /foo/bar/bazel.
    fdio_spawn_action_t action;
    action.action = FDIO_SPAWN_ACTION_CLONE_DIR;
    action.dir.prefix = "/foo/bar/baz";

    zx::channel h1, h2;
    ASSERT_OK(zx::channel::create(0, &h1, &h2));
    fdio_ns_t* ns = nullptr;
    ASSERT_OK(fdio_ns_get_installed(&ns));
    ASSERT_OK(fdio_ns_bind(ns, "/foo/bar/bazel", h1.release()));

    const char* argv[] = {bin_path, "--stat", "/foo/bar", nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL_EXCEPT_NS, bin_path, argv,
                            nullptr, 1, &action, process.reset_and_get_address(), err_msg);
    EXPECT_EQ(ZX_OK, status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(return_code, -6);

    // Unbind the test namespace.
    EXPECT_EQ(ZX_OK, fdio_ns_unbind(ns, "/foo/bar/bazel"));
  }

  {
    // Same as above but the prefix does not exist in our namespace. The fdio_spawn_etc should
    // succeed but the new process should not see any namespaces under that path.
    fdio_spawn_action_t action;
    action.action = FDIO_SPAWN_ACTION_CLONE_DIR;
    action.dir.prefix = "/foo/bar/baz";

    const char* argv[] = {bin_path, "--action", "ns-entry", nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL_EXCEPT_NS, bin_path, argv,
                            nullptr, 1, &action, process.reset_and_get_address(), err_msg);
    ASSERT_OK(status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(return_code, -4);
  }
}

TEST(SpawnTest, SpawnErrors) {
  zx_status_t status;
  zx::process process;
  int64_t return_code;
  const char* bin_path = kSpawnChild;
  char err_msg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];
  char err_msg2[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];
  const char* argv[] = {bin_path, nullptr};

  ASSERT_EQ(ZX_ERR_INVALID_ARGS, fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path,
                                            nullptr, process.reset_and_get_address()));

  ASSERT_EQ(ZX_ERR_INVALID_ARGS,
            fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, nullptr, 1,
                           nullptr, process.reset_and_get_address(), err_msg2));

  {
    fdio_spawn_action_t action;
    action.action = FDIO_SPAWN_ACTION_ADD_NS_ENTRY;
    action.ns.prefix = "/foo/bar/baz";
    action.ns.handle = ZX_HANDLE_INVALID;

    ASSERT_EQ(ZX_ERR_INVALID_ARGS,
              fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, nullptr, 1,
                             &action, process.reset_and_get_address(), err_msg2));
  }

  {
    fdio_spawn_action_t action;
    action.action = FDIO_SPAWN_ACTION_ADD_HANDLE;
    action.h.id = PA_USER0;
    action.h.handle = ZX_HANDLE_INVALID;

    ASSERT_EQ(ZX_ERR_INVALID_ARGS,
              fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, nullptr, 1,
                             &action, process.reset_and_get_address(), err_msg2));
  }

  {
    fdio_spawn_action_t action;
    action.action = FDIO_SPAWN_ACTION_SET_NAME;
    action.name.data = nullptr;

    ASSERT_EQ(ZX_ERR_INVALID_ARGS,
              fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, nullptr, 1,
                             &action, process.reset_and_get_address(), err_msg2));
  }

  {
    const char* empty_argv[] = {nullptr};
    ASSERT_EQ(ZX_ERR_INVALID_ARGS, fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path,
                                              empty_argv, process.reset_and_get_address()));
  }

  ASSERT_EQ(ZX_ERR_NOT_FOUND,
            fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, "/bogus/path", argv, nullptr, 0,
                           nullptr, process.reset_and_get_address(), err_msg));
  EXPECT_TRUE(strstr(err_msg, "/bogus/path") != nullptr);

  {
    zx::job job;
    ASSERT_OK(zx::job::default_job()->duplicate(0, &job));
    ASSERT_EQ(ZX_ERR_ACCESS_DENIED, fdio_spawn(job.get(), FDIO_SPAWN_CLONE_ALL, bin_path, argv,
                                               process.reset_and_get_address()));
  }

  {
    ASSERT_EQ(30, dup2(0, 30));
    ASSERT_EQ(0, close(0));
    status = fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv,
                        process.reset_and_get_address());
    ASSERT_OK(status);
    join(process, &return_code);
    EXPECT_EQ(return_code, 43);
    ASSERT_EQ(0, dup2(30, 0));
    ASSERT_EQ(0, close(30));
  }

  {
    ASSERT_EQ(30, dup2(0, 30));
    ASSERT_EQ(0, close(0));
    zxio_storage_t* storage = nullptr;
    fdio_t* io = fdio_zxio_create(&storage);
    ASSERT_EQ(0, fdio_bind_to_fd(io, 0, 0));
    status = fdio_spawn(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv,
                        process.reset_and_get_address());
    ASSERT_OK(status);
    join(process, &return_code);
    EXPECT_EQ(return_code, 43);
    ASSERT_EQ(0, close(0));
    ASSERT_EQ(0, dup2(30, 0));
    ASSERT_EQ(0, close(30));
  }

  {
    zxio_storage_t* storage = nullptr;
    fdio_t* io = fdio_zxio_create(&storage);
    int fd = fdio_bind_to_fd(io, -1, 0);
    ASSERT_GE(fd, 3);

    fdio_spawn_action_t action;
    action.action = FDIO_SPAWN_ACTION_CLONE_FD;
    action.fd.local_fd = fd;
    action.fd.target_fd = 21;

    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, nullptr, 1,
                            &action, process.reset_and_get_address(), err_msg);
    ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, status, "%s", err_msg);
    ASSERT_EQ(0, close(fd));
  }

  {
    zxio_storage_t* storage = nullptr;
    fdio_t* io = fdio_zxio_create(&storage);
    int fd = fdio_bind_to_fd(io, -1, 0);
    ASSERT_GE(fd, 3);

    fdio_spawn_action_t action;
    action.action = FDIO_SPAWN_ACTION_TRANSFER_FD;
    action.fd.local_fd = fd;
    action.fd.target_fd = 21;

    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, nullptr, 1,
                            &action, process.reset_and_get_address(), err_msg);
    ASSERT_EQ(ZX_ERR_NOT_SUPPORTED, status, "%s", err_msg);
    ASSERT_EQ(-1, close(fd));
  }

  {
    zxio_storage_t* storage = nullptr;
    fdio_t* io = fdio_zxio_create(&storage);
    int fd = fdio_bind_to_fd(io, -1, 0);
    ASSERT_GE(fd, 3);

    fdio_spawn_action_t actions[2];
    actions[0].action = FDIO_SPAWN_ACTION_ADD_HANDLE;
    // Adding an invalid handle is an error.
    actions[0].h.handle = ZX_HANDLE_INVALID;
    actions[0].h.id = PA_FD;
    actions[1].action = FDIO_SPAWN_ACTION_TRANSFER_FD;
    actions[1].fd.local_fd = fd;
    actions[1].fd.target_fd = 21;

    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, nullptr, 2,
                            actions, process.reset_and_get_address(), err_msg);
    ASSERT_EQ(ZX_ERR_INVALID_ARGS, status, "%s", err_msg);
    ASSERT_EQ(-1, close(fd));
  }

  {
    // FDIO_SPAWN_ACTION_CLONE_DIR with trailing '/' should be rejected.
    fdio_spawn_action_t action;
    action.action = FDIO_SPAWN_ACTION_CLONE_DIR;
    action.dir.prefix = "/foo/bar/baz/";

    zx::channel h1, h2;
    ASSERT_OK(zx::channel::create(0, &h1, &h2));
    fdio_ns_t* ns = nullptr;
    ASSERT_OK(fdio_ns_get_installed(&ns));
    ASSERT_OK(fdio_ns_bind(ns, "/foo/bar/baz", h1.release()));

    const char* argv[] = {bin_path, "--action", "ns-entry", nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL_EXCEPT_NS, bin_path, argv,
                            nullptr, 1, &action, process.reset_and_get_address(), err_msg);
    EXPECT_EQ(ZX_ERR_INVALID_ARGS, status, "%s", err_msg);

    // Unbind the test namespace.
    EXPECT_EQ(ZX_OK, fdio_ns_unbind(ns, "/foo/bar/baz"));
  }

  {
    ASSERT_EQ(0, dup2(1, 0));
    ASSERT_EQ(30, dup2(0, 30));
    ASSERT_EQ(31, dup2(0, 31));
    const fdio_spawn_action_t actions[] = {
        {.action = FDIO_SPAWN_ACTION_CLONE_FD, .fd = {.local_fd = 30, .target_fd = 1}},
        {.action = FDIO_SPAWN_ACTION_CLONE_FD, .fd = {.local_fd = 31, .target_fd = 1}},
    };
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, nullptr,
                            std::size(actions), actions, process.reset_and_get_address(), err_msg);
    EXPECT_EQ(ZX_ERR_ALREADY_EXISTS, status, "%s", err_msg);
    ASSERT_EQ(0, close(30));
    ASSERT_EQ(0, close(31));
  }

  {
    ASSERT_EQ(30, dup2(0, 30));
    const fdio_spawn_action_t actions[] = {
        {.action = FDIO_SPAWN_ACTION_CLONE_FD, .fd = {.local_fd = 30, .target_fd = FDIO_MAX_FD}},
    };
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, nullptr,
                            std::size(actions), actions, process.reset_and_get_address(), err_msg);
    EXPECT_EQ(ZX_ERR_OUT_OF_RANGE, status, "%s", err_msg);
    ASSERT_EQ(0, close(30));
  }
}

TEST(SpawnTest, SpawnVmo) {
  zx_status_t status;
  zx::process process;
  int64_t return_code;
  const char* bin_path = kSpawnChild;
  char err_msg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];

  {
    int fd;
    zx::vmo vmo;
    ASSERT_OK(fdio_open_fd(
        bin_path,
        static_cast<uint32_t>(fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_EXECUTABLE),
        &fd));
    ASSERT_GE(fd, 0);
    ASSERT_OK(fdio_get_vmo_exec(fd, vmo.reset_and_get_address()));
    close(fd);

    const char* argv[] = {bin_path, nullptr};
    status = fdio_spawn_vmo(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, vmo.release(), argv, nullptr,
                            0, nullptr, process.reset_and_get_address(), err_msg);
    ASSERT_OK(status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(return_code, 43);
  }
}

class SpawnUtcClockTest : public ::zxtest::Test {
 protected:
  void SetUp() override {
    // Remove the current global clock. It will be restored at the end of this test.
    ASSERT_OK(zx_utc_reference_swap(ZX_HANDLE_INVALID, &prev_clock_));
  }

  void TearDown() override {
    zx_handle_t dummy;
    ASSERT_OK(zx_utc_reference_swap(prev_clock_, &dummy));
  }

  zx_koid_t GetKoid(const zx::clock& clock) {
    zx_info_handle_basic_t info;
    zx_status_t status =
        clock.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr);
    if (status != ZX_OK) {
      ADD_FAILURE("failed to get koid of handle");
      return ZX_KOID_INVALID;
    }
    return info.koid;
  }

 private:
  zx_handle_t prev_clock_ = ZX_HANDLE_INVALID;
};

TEST_F(SpawnUtcClockTest, NoGlobalUtcClock) {
  zx_status_t status;
  zx::process process;
  int64_t return_code;
  const char* bin_path = kSpawnChild;
  char err_msg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];

  {
    const char* argv[] = {bin_path, "--action", "add-handle-clock-utc", nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, nullptr, 0,
                            nullptr, process.reset_and_get_address(), err_msg);
    ASSERT_OK(status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(static_cast<zx_koid_t>(return_code), ZX_KOID_INVALID);
  }
}

TEST_F(SpawnUtcClockTest, SendGlobalUtcClock) {
  zx_status_t status;
  zx::process process;
  int64_t return_code;
  const char* bin_path = kSpawnChild;
  char err_msg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];

  // Set a global UTC clock.
  zx::clock test_clock;
  zx_handle_t prev_clock;
  ASSERT_OK(zx::clock::create(0, nullptr, &test_clock));
  ASSERT_OK(zx_utc_reference_swap(test_clock.get_handle(), &prev_clock));
  {
    const char* argv[] = {bin_path, "--action", "add-handle-clock-utc", nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, nullptr, 0,
                            nullptr, process.reset_and_get_address(), err_msg);
    ASSERT_OK(status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(static_cast<zx_koid_t>(return_code), GetKoid(test_clock));
  }
}

TEST_F(SpawnUtcClockTest, SendExplicitUtcClock) {
  zx_status_t status;
  zx::process process;
  int64_t return_code;
  const char* bin_path = kSpawnChild;
  char err_msg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];

  {
    zx::clock test_clock;
    ASSERT_OK(zx::clock::create(0, nullptr, &test_clock));
    zx_koid_t expected_koid = GetKoid(test_clock);

    fdio_spawn_action_t action;
    action.action = FDIO_SPAWN_ACTION_ADD_HANDLE;
    action.h.id = PA_CLOCK_UTC;
    action.h.handle = test_clock.release();

    const char* argv[] = {bin_path, "--action", "add-handle-clock-utc", nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL & ~FDIO_SPAWN_CLONE_UTC_CLOCK,
                            bin_path, argv, nullptr, 1, &action, process.reset_and_get_address(),
                            err_msg);
    ASSERT_OK(status, "%s", err_msg);
    join(process, &return_code);
    EXPECT_EQ(static_cast<zx_koid_t>(return_code), expected_koid);
  }
}

TEST_F(SpawnUtcClockTest, CannotCloneGlobalAndSendExplicit) {
  zx_status_t status;
  zx::process process;
  const char* bin_path = kSpawnChild;
  char err_msg[FDIO_SPAWN_ERR_MSG_MAX_LENGTH];

  {
    zx::clock test_clock;
    ASSERT_OK(zx::clock::create(0, nullptr, &test_clock));

    fdio_spawn_action_t action;
    action.action = FDIO_SPAWN_ACTION_ADD_HANDLE;
    action.h.id = PA_CLOCK_UTC;
    action.h.handle = test_clock.release();

    const char* argv[] = {bin_path, "--action", "add-handle-clock-utc", nullptr};
    status = fdio_spawn_etc(ZX_HANDLE_INVALID, FDIO_SPAWN_CLONE_ALL, bin_path, argv, nullptr, 1,
                            &action, process.reset_and_get_address(), err_msg);
    ASSERT_NOT_OK(status);
  }
}

}  // namespace
