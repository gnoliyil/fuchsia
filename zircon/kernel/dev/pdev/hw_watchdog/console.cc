// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#if LK_DEBUGLEVEL > 1

#include <lib/console.h>
#include <lib/debuglog.h>
#include <platform.h>

#include <dev/hw_watchdog.h>
#include <platform/halt_helper.h>

static void usage(const char* cmd_name) {
  printf("Usage:\n");
  printf("%s status   : show the recent status of the hardware watchdog subsystem.\n", cmd_name);
  printf("%s pet      : force an immediate pet of the watchdog.\n", cmd_name);
  printf("%s enable   : attempt to enable the watchdog.\n", cmd_name);
  printf("%s disable  : attempt to disable the watchdog.\n", cmd_name);
  printf("%s force    : force the watchdog to fire by locking up all cores.\n", cmd_name);
  printf("%s suppress : Pet the WDT one last time, then suppress future pets.\n", cmd_name);
  printf("%s help     : show this message.\n", cmd_name);
}

enum class Cmd {
  Status,
  Pet,
  Enable,
  Disable,
  Force,
  Suppress,
};

static int cmd_watchdog(int argc, const cmd_args* argv, uint32_t flags) {
  if (argc < 2) {
    printf("Not enough arguments.\n");
    usage(argv[0].str);
    return -1;
  }

  Cmd command;
  if (!strcmp(argv[1].str, "status")) {
    command = Cmd::Status;
  } else if (!strcmp(argv[1].str, "pet")) {
    command = Cmd::Pet;
  } else if (!strcmp(argv[1].str, "enable")) {
    command = Cmd::Enable;
  } else if (!strcmp(argv[1].str, "disable")) {
    command = Cmd::Disable;
  } else if (!strcmp(argv[1].str, "force")) {
    command = Cmd::Force;
  } else if (!strcmp(argv[1].str, "suppress")) {
    command = Cmd::Suppress;
  } else if (!strcmp(argv[1].str, "help")) {
    usage(argv[0].str);
    return 0;
  } else {
    printf("Unrecognized command.\n");
    usage(argv[0].str);
    return -1;
  }

  if (!hw_watchdog_present()) {
    printf("There is no hardware watchdog present in this system.\n");
    return 0;
  }

  switch (command) {
    case Cmd::Status: {
      zx_time_t last_pet = hw_watchdog_get_last_pet_time();
      zx_time_t now = current_time();
      printf("Enabled  : %s\n", hw_watchdog_is_enabled() ? "yes" : "no");
      printf("Timeout  : %ld mSec\n", hw_watchdog_get_timeout_nsec() / ZX_MSEC(1));
      printf("Last Pet : %ld.%03ld (%ld mSec ago)\n", last_pet / ZX_SEC(1),
             (last_pet / ZX_MSEC(1)) % 1000, (now - last_pet) / ZX_MSEC(1));
      printf("Petting  : %s\n", hw_watchdog_is_petting_suppressed() ? "Suppressed" : "Enabled");
    } break;

    case Cmd::Pet: {
      printf("Watchdog has been pet.  They're a good dog! (yes they are!!)\n");
    } break;

    case Cmd::Enable: {
      zx_status_t res;
      res = hw_watchdog_set_enabled(true);
      if (res == ZX_ERR_NOT_SUPPORTED) {
        printf("Watchdog does not support enabling.\n");
        return res;
      } else if (res != ZX_OK) {
        printf("Error enabling watchdog (%d)\n", res);
        return res;
      } else {
        printf("Watchdog enabled.\n");
      }
    } break;

    case Cmd::Disable: {
      zx_status_t res;
      res = hw_watchdog_set_enabled(false);
      if (res == ZX_ERR_NOT_SUPPORTED) {
        printf("Watchdog does not support disabling.\n");
        return res;
      } else if (res != ZX_OK) {
        printf("Error disabling watchdog (%d)\n", res);
        return res;
      } else {
        printf("Watchdog disabled.\n");
      }
    } break;

    case Cmd::Force: {
      if (!hw_watchdog_is_enabled()) {
        printf("Watchdog is not enabled.  Enable the watchdog first.\n");
        return ZX_ERR_BAD_STATE;
      }

      // In order to _really_ wedge the system we...
      // 1) Migrate our thread to the boot core. (this pins the thread there too)
      // 2) Halt all of the secondary cores
      // 3) Disable interrupts.
      // 4) Spin forever.
      //
      Thread::Current::MigrateToCpu(BOOT_CPU_ID);
      [[maybe_unused]] zx_status_t status = platform_halt_secondary_cpus(ZX_TIME_INFINITE);
      DEBUG_ASSERT(status == ZX_OK);

      arch_disable_ints();

      // Make sure that our printf goes directly to the UART, bypassing any
      // buffering which is not going to get drained now that we have stopped
      // the system.
      dlog_panic_start();

      zx_time_t deadline =
          zx_time_add_duration(hw_watchdog_get_last_pet_time(), hw_watchdog_get_timeout_nsec());
      printf("System wedged!  Watchdog will fire in %ld mSec\n",
             (deadline - current_time()) / ZX_MSEC(1));

      // Spin forever.  The watchdog should reboot us.
      while (true)
        ;
    } break;

    case Cmd::Suppress: {
      hw_watchdog_pet();
      hw_watchdog_suppress_petting(true);

      if (!hw_watchdog_is_enabled()) {
        printf("WDT petting is now suppressed, but the WDT is not currently enabled.\n");
      } else {
        zx_time_t deadline =
            zx_time_add_duration(hw_watchdog_get_last_pet_time(), hw_watchdog_get_timeout_nsec());
        printf("WDT petting is now suppressed, WDT will fire in %ld mSec\n",
               (deadline - current_time()) / ZX_MSEC(1));
      }
    } break;
  };

  return 0;
}

STATIC_COMMAND_START
STATIC_COMMAND("wdt", "hardware watchdog commands", &cmd_watchdog)
STATIC_COMMAND_END(gfx)

#endif
